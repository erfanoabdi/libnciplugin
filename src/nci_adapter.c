/*
 * Copyright (C) 2019-2020 Jolla Ltd.
 * Copyright (C) 2019-2020 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "nci_adapter_impl.h"
#include "nci_plugin_p.h"
#include "nci_plugin_log.h"

#include <nfc_adapter_impl.h>
#include <nfc_target_impl.h>
#include <nfc_tag_t2.h>
#include <nfc_tag_t4.h>

#include <nci_core.h>

GLOG_MODULE_DEFINE("nciplugin");

/* NCI core events */
enum {
    CORE_EVENT_CURRENT_STATE,
    CORE_EVENT_NEXT_STATE,
    CORE_EVENT_INTF_ACTIVATED,
    CORE_EVENT_COUNT
};

struct nci_adapter_priv {
    gulong nci_event_id[CORE_EVENT_COUNT];
    NFC_MODE desired_mode;
    NFC_MODE current_mode;
    gboolean mode_change_pending;
    guint mode_check_id;
    guint presence_check_id;
    guint presence_check_timer;
};

G_DEFINE_ABSTRACT_TYPE(NciAdapter, nci_adapter, NFC_TYPE_ADAPTER)
#define NCI_ADAPTER_GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS((obj), \
        NCI_TYPE_ADAPTER, NciAdapterClass)
#define SUPER_CLASS nci_adapter_parent_class

#define PRESENCE_CHECK_PERIOD_MS (250)

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
nci_adapter_drop_target(
    NciAdapter* self)
{
    NfcTarget* target = self->target;

    if (target) {
        NciAdapterPriv* priv = self->priv;

        self->target = NULL;
        if (priv->presence_check_timer) {
            g_source_remove(priv->presence_check_timer);
            priv->presence_check_timer = 0;
        }
        if (priv->presence_check_id) {
            nfc_target_cancel_transmit(target, priv->presence_check_id);
            priv->presence_check_id = 0;
        }
        GINFO("Target is gone");
        nfc_target_gone(target);
        nfc_target_unref(target);
    }
}

static
void
nci_adapter_presence_check_done(
    NfcTarget* target,
    gboolean ok,
    void* user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterPriv* priv = self->priv;

    GDEBUG("Presence check %s", ok ? "ok" : "failed");
    priv->presence_check_id = 0;
    if (!ok) {
        nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
    }
}

static
gboolean
nci_adapter_presence_check_timer(
    gpointer user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterPriv* priv = self->priv;

    if (!priv->presence_check_id && !self->target->sequence) {
        priv->presence_check_id = nci_target_presence_check(self->target,
            nci_adapter_presence_check_done, self);
        if (!priv->presence_check_id) {
            GDEBUG("Failed to start presence check");
            priv->presence_check_timer = 0;
            nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
            return G_SOURCE_REMOVE;
        }
    } else {
        GDEBUG("Skipped presence check");
    }
    return G_SOURCE_CONTINUE;
}

static
void
nci_adapter_mode_check(
    NciAdapter* self)
{
    NciCore* nci = self->nci;
    NciAdapterPriv* priv = self->priv;
    const NFC_MODE mode = (nci->current_state > NCI_RFST_IDLE) ?
        NFC_MODE_READER_WRITER : NFC_MODE_NONE;

    if (priv->mode_check_id) {
        g_source_remove(priv->mode_check_id);
        priv->mode_check_id = 0;
    }
    if (priv->mode_change_pending) {
        if (mode == priv->desired_mode) {
            priv->mode_change_pending = FALSE;
            priv->current_mode = mode;
            nfc_adapter_mode_notify(NFC_ADAPTER(self), mode, TRUE);
        }
    } else if (priv->current_mode != mode) {
        priv->current_mode = mode;
        nfc_adapter_mode_notify(NFC_ADAPTER(self), mode, FALSE);
    }
}

gboolean
nci_adapter_mode_check_cb(
    gpointer user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterPriv* priv = self->priv;

    priv->mode_check_id = 0;
    nci_adapter_mode_check(self);
    return G_SOURCE_REMOVE;
}

static
void
nci_adapter_schedule_mode_check(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    if (!priv->mode_check_id) {
        priv->mode_check_id = g_idle_add(nci_adapter_mode_check_cb, self);
    }
}

static
const NfcParamPollA*
nci_adapter_convert_poll_a(
    NfcParamPollA* dest,
    const NciModeParamPollA* src)
{
    dest->sel_res = src->sel_res;
    dest->nfcid1.bytes = src->nfcid1;
    dest->nfcid1.size = src->nfcid1_len;
    return dest;
}

static
const NfcParamPollB*
nci_adapter_convert_poll_b(
    NfcParamPollB* dest,
    const NciModeParamPollB* src)
{
    dest->fsc = src->fsc;
    dest->nfcid0.bytes = src->nfcid0;
    dest->nfcid0.size = sizeof(src->nfcid0);
    return dest;
}

static
const NfcParamIsoDepPollA*
nci_adapter_convert_iso_dep_poll_a(
    NfcParamIsoDepPollA* dest,
    const NciActivationParamIsoDepPollA* src)
{
    dest->fsc = src->fsc;
    dest->t1 = src->t1;
    return dest;
}

static
void
nci_adapter_nci_intf_activated(
    NciCore* nci,
    const NciIntfActivationNtf* ntf,
    void* user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterPriv* priv = self->priv;
    const NciModeParam* mp = ntf->mode_param;
    NfcTag* tag = NULL;

    /* Drop the previous target, if any */
    nci_adapter_drop_target(self);

    /* Register the new tag */
    self->target = nci_target_new(nci, ntf);

    /* Figure out what kind of target we are dealing with */
    if (mp) {
        NfcParamPollA poll_a;
        NfcParamPollB poll_b;

        switch (ntf->mode) {
        case NCI_MODE_PASSIVE_POLL_A:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_FRAME:
                /* Type 2 Tag */
                tag = nfc_adapter_add_tag_t2(NFC_ADAPTER(self), self->target,
                    nci_adapter_convert_poll_a(&poll_a, &mp->poll_a));
                break;
            case NCI_RF_INTERFACE_ISO_DEP:
                /* ISO-DEP Type 4A */
                if (ntf->activation_param) {
                    const NciActivationParam* ap = ntf->activation_param;
                    NfcParamIsoDepPollA iso_dep_poll_a;

                    tag = nfc_adapter_add_tag_t4a(NFC_ADAPTER(self),
                        self->target, nci_adapter_convert_poll_a
                            (&poll_a, &mp->poll_a),
                        nci_adapter_convert_iso_dep_poll_a
                            (&iso_dep_poll_a, &ap->iso_dep_poll_a));
                }
                break;
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
                break;
            }
            break;
        case NCI_MODE_PASSIVE_POLL_B:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_ISO_DEP:
                /* ISO-DEP Type 4B */
                tag = nfc_adapter_add_tag_t4b(NFC_ADAPTER(self), self->target,
                    nci_adapter_convert_poll_b(&poll_b, &mp->poll_b), NULL);
                break;
            case NCI_RF_INTERFACE_FRAME:
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
                break;
            }
            break;
        case NCI_MODE_ACTIVE_POLL_A:
        case NCI_MODE_PASSIVE_POLL_F:
        case NCI_MODE_ACTIVE_POLL_F:
        case NCI_MODE_PASSIVE_POLL_15693:
        case NCI_MODE_PASSIVE_LISTEN_A:
        case NCI_MODE_PASSIVE_LISTEN_B:
        case NCI_MODE_PASSIVE_LISTEN_F:
        case NCI_MODE_ACTIVE_LISTEN_A:
        case NCI_MODE_ACTIVE_LISTEN_F:
        case NCI_MODE_PASSIVE_LISTEN_15693:
            break;
        }
    }

    if (!tag) {
        nfc_adapter_add_other_tag(NFC_ADAPTER(self), self->target);
    }

    /* Start periodic presence checks */
    priv->presence_check_timer = g_timeout_add(PRESENCE_CHECK_PERIOD_MS,
        nci_adapter_presence_check_timer, self);
}

static
void
nci_adapter_nci_next_state_changed(
    NciCore* nci,
    void* user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterClass* klass = NCI_ADAPTER_GET_CLASS(self);

    klass->next_state_changed(self);
}

static
void
nci_adapter_nci_current_state_changed(
    NciCore* nci,
    void* user_data)
{
    NciAdapter* self = NCI_ADAPTER(user_data);
    NciAdapterClass* klass = NCI_ADAPTER_GET_CLASS(self);

    klass->current_state_changed(self);
}

/*==========================================================================*
 * Interface
 *==========================================================================*/

void
nci_adapter_init_base(
    NciAdapter* self,
    NciHalIo* io)
{
    NciAdapterPriv* priv = self->priv;

    self->nci = nci_core_new(io);
    priv->nci_event_id[CORE_EVENT_CURRENT_STATE] =
        nci_core_add_current_state_changed_handler(self->nci,
            nci_adapter_nci_current_state_changed, self);
    priv->nci_event_id[CORE_EVENT_NEXT_STATE] =
        nci_core_add_next_state_changed_handler(self->nci,
            nci_adapter_nci_next_state_changed, self);
    priv->nci_event_id[CORE_EVENT_INTF_ACTIVATED] =
        nci_core_add_intf_activated_handler(self->nci,
            nci_adapter_nci_intf_activated, self);
}

/*
 * This is supposed to be called from finalize method of the derived class
 * to make sure that NciCore is freed before NciHalIo in cases when NciHalIo
 * is allocated dynamically. Note that in that case it will be called twice,
 * once by the derived class and once by nci_adapter_finalize.
 */
void
nci_adapter_finalize_core(
    NciAdapter* self)
{
    NciAdapterPriv* priv = self->priv;

    if (priv->mode_check_id) {
        g_source_remove(priv->mode_check_id);
        priv->mode_check_id = 0;
    }
    if (self->nci) {
        nci_core_remove_all_handlers(self->nci, priv->nci_event_id);
        nci_core_free(self->nci);
        self->nci = NULL;
    }
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
nci_adapter_submit_mode_request(
    NfcAdapter* adapter,
    NFC_MODE mode)
{
    NciAdapter* self = NCI_ADAPTER(adapter);
    NciAdapterPriv* priv = self->priv;

    priv->desired_mode = mode;
    priv->mode_change_pending = TRUE;
    nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
    nci_adapter_schedule_mode_check(self);
    return TRUE;
}

static
void
nci_adapter_cancel_mode_request(
    NfcAdapter* adapter)
{
    NciAdapter* self = NCI_ADAPTER(adapter);
    NciAdapterPriv* priv = self->priv;

    priv->mode_change_pending = FALSE;
    nci_adapter_schedule_mode_check(self);
}

static
void
nci_adapter_current_state_changed(
    NciAdapter* adapter)
{
    nci_adapter_mode_check(NCI_ADAPTER(adapter));
}

static
void
nci_adapter_next_state_changed(
    NciAdapter* adapter)
{
    NciAdapter* self = NCI_ADAPTER(adapter);

    if (self->nci->next_state != NCI_RFST_POLL_ACTIVE) {
        nci_adapter_drop_target(self);
    }
    nci_adapter_mode_check(self);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nci_adapter_init(
    NciAdapter* self)
{
    NfcAdapter* adapter = NFC_ADAPTER(self);
    NciAdapterPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE(self, NCI_TYPE_ADAPTER,
        NciAdapterPriv);

    self->priv = priv;
    adapter->supported_modes = NFC_MODE_READER_WRITER;
    adapter->supported_tags = NFC_TAG_TYPE_MIFARE_ULTRALIGHT;
    adapter->supported_protocols =  NFC_PROTOCOL_T2_TAG |
        NFC_PROTOCOL_T4A_TAG | NFC_PROTOCOL_T4B_TAG;
}

static
void
nci_adapter_dispose(
    GObject* object)
{
    nci_adapter_drop_target(NCI_ADAPTER(object));
    G_OBJECT_CLASS(SUPER_CLASS)->dispose(object);
}

static
void
nci_adapter_finalize(
    GObject* object)
{
    nci_adapter_finalize_core(NCI_ADAPTER(object));
    G_OBJECT_CLASS(SUPER_CLASS)->finalize(object);
}

static
void
nci_adapter_class_init(
    NciAdapterClass* klass)
{
    NfcAdapterClass* adapter_class = NFC_ADAPTER_CLASS(klass);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(NciAdapterPriv));
    klass->current_state_changed = nci_adapter_current_state_changed;
    klass->next_state_changed = nci_adapter_next_state_changed;
    adapter_class->submit_mode_request = nci_adapter_submit_mode_request;
    adapter_class->cancel_mode_request = nci_adapter_cancel_mode_request;
    object_class->dispose = nci_adapter_dispose;
    object_class->finalize = nci_adapter_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
