/*
 * Copyright (C) 2019-2020 Jolla Ltd.
 * Copyright (C) 2019-2020 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2020 Open Mobile Platform LLC.
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

#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include "nci_adapter_impl.h"
#include "nci_plugin_p.h"
#include "nci_plugin_log.h"

#include <nfc_adapter_impl.h>
#include <nfc_target_impl.h>
#include <nfc_tag_t2.h>
#include <nfc_tag_t4.h>

#include <nci_core.h>
#include <nci_util.h>

#include <gutil_misc.h>
#include <gutil_macros.h>

GLOG_MODULE_DEFINE("nciplugin");

/* NCI core events */
enum {
    CORE_EVENT_CURRENT_STATE,
    CORE_EVENT_NEXT_STATE,
    CORE_EVENT_INTF_ACTIVATED,
    CORE_EVENT_COUNT
};

typedef struct nci_adapter_intf_info {
    NCI_RF_INTERFACE rf_intf;
    NCI_PROTOCOL protocol;
    NCI_MODE mode;
    GUtilData mode_param;
    GUtilData activation_param;
    NciModeParam* mode_param_parsed;
} NciAdapterIntfInfo;

struct nci_adapter_priv {
    gulong nci_event_id[CORE_EVENT_COUNT];
    NFC_MODE desired_mode;
    NFC_MODE current_mode;
    gboolean mode_change_pending;
    guint mode_check_id;
    guint presence_check_id;
    guint presence_check_timer;
    NciAdapterIntfInfo* active_intf;
    gboolean reactivating;
};

G_DEFINE_ABSTRACT_TYPE(NciAdapter, nci_adapter, NFC_TYPE_ADAPTER)
#define NCI_ADAPTER_GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS((obj), \
        NCI_TYPE_ADAPTER, NciAdapterClass)
#define SUPER_CLASS nci_adapter_parent_class

#define PRESENCE_CHECK_PERIOD_MS (250)

#define RANDOM_UID_SIZE (4)
#define RANDOM_UID_START_BYTE (0x08)

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
NciAdapterIntfInfo*
nci_adapter_intf_info_new(
    const NciIntfActivationNtf* ntf)
{
    if (ntf) {
        /* Allocate the whole thing from a single memory block */
        const gsize total = G_ALIGN8(sizeof(NciAdapterIntfInfo)) +
            G_ALIGN8(ntf->mode_param_len) + ntf->activation_param_len;
        NciAdapterIntfInfo* info = g_malloc(total);
        guint8* ptr = (guint8*)info;

        info->rf_intf = ntf->rf_intf;
        info->protocol = ntf->protocol;
        info->mode = ntf->mode;
        ptr += G_ALIGN8(sizeof(NciAdapterIntfInfo));

        info->mode_param.size = ntf->mode_param_len;
        if (ntf->mode_param_len) {
            info->mode_param.bytes = ptr;
            memcpy(ptr, ntf->mode_param_bytes, ntf->mode_param_len);
            ptr += G_ALIGN8(ntf->mode_param_len);
        } else {
            info->mode_param.bytes = NULL;
        }

        info->activation_param.size = ntf->activation_param_len;
        if (ntf->activation_param_len) {
            info->activation_param.bytes = ptr;
            memcpy(ptr, ntf->activation_param_bytes, ntf->activation_param_len);
        } else {
            info->activation_param.bytes = NULL;
        }

        info->mode_param_parsed = nci_util_copy_mode_param(ntf->mode_param,
            ntf->mode);

        return info;
    }
    return NULL;
}

static
gboolean
mode_param_match_poll_a(
    const NciModeParamPollA* pa1,
    const NciModeParamPollA* pa2)
{
    /*
    * Compare all fields except UID 'cause UID may be
    * changed after losing field
    */
    return pa1->sel_res == pa2->sel_res &&
        pa1->sel_res_len == pa2->sel_res_len &&
        !memcmp(pa1->sens_res, pa2->sens_res, sizeof(pa2->sens_res));
}

static
gboolean
mode_param_match_poll_b(
    const NciModeParamPollB* pb1,
    const NciModeParamPollB* pb2)
{
    /*
    * Compare all fields except UID 'cause UID may be
    * changed after losing field
    */
    return pb1->fsc == pb2->fsc &&
        !memcmp(pb1->app_data, pb2->app_data, sizeof(pb2->app_data)) &&
        pb1->prot_info.size == pb2->prot_info.size &&
        gutil_data_equal(&pb1->prot_info, &pb2->prot_info);
}

static
gboolean
mode_param_match_poll_a_t2(
    const NciModeParamPollA* pa1,
    const NciModeParamPollA* pa2)
{
    gboolean partial_match = mode_param_match_poll_a(pa1, pa2);

    /*
    * For tag type 2 logic is almost the same, but random UID has some
    * limitations: according to AN10927 Random UID RID should be handled
    * separately - single sized (4 bytes) starting with 0x08
    */
    if (pa1->nfcid1_len == pa2->nfcid1_len &&
        pa2->nfcid1_len == RANDOM_UID_SIZE &&
        pa1->nfcid1[0] == pa2->nfcid1[0] &&
        pa2->nfcid1[0] == RANDOM_UID_START_BYTE) {
        return partial_match;
    } else {
        /* Otherwise UID should fully match */
        return partial_match &&
            pa1->nfcid1_len == pa2->nfcid1_len &&
            !memcmp(pa1->nfcid1, pa2->nfcid1, pa2->nfcid1_len);
    }
}

static
gboolean
nci_adapter_info_mode_params_matches(
    const NciAdapterIntfInfo* info,
    const NciIntfActivationNtf* ntf)
{
    const NciModeParam* mp1 = info->mode_param_parsed;
    const NciModeParam* mp2 = ntf->mode_param;

    if (mp1 && mp2) {
        /* Mode params criteria depends on type of tag */
        switch (ntf->mode) {
        case NCI_MODE_PASSIVE_POLL_A:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_FRAME:
                /* Type 2 Tag */
                return mode_param_match_poll_a_t2(&mp1->poll_a, &mp2->poll_a);
            case NCI_RF_INTERFACE_ISO_DEP:
                /* ISO-DEP Type 4A */
                return mode_param_match_poll_a(&mp1->poll_a, &mp2->poll_a);
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
            case NCI_RF_INTERFACE_PROPRIETARY:
                break;
            }
            break;
        case NCI_MODE_PASSIVE_POLL_B:
            switch (ntf->rf_intf) {
            case NCI_RF_INTERFACE_ISO_DEP:
                /* ISO-DEP Type 4B */
                return mode_param_match_poll_b(&mp1->poll_b, &mp2->poll_b);
            case NCI_RF_INTERFACE_FRAME:
            case NCI_RF_INTERFACE_NFCEE_DIRECT:
            case NCI_RF_INTERFACE_NFC_DEP:
            case NCI_RF_INTERFACE_PROPRIETARY:
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
    /* Full match is expected in other cases */
    return info->mode_param.size == ntf->mode_param_len &&
        (!ntf->mode_param_len || !memcmp(info->mode_param.bytes,
            ntf->mode_param_bytes, ntf->mode_param_len));
}

static
gboolean
nci_adapter_intf_info_matches(
    const NciAdapterIntfInfo* info,
    const NciIntfActivationNtf* ntf)
{
    return info &&
        info->rf_intf == ntf->rf_intf &&
        info->protocol == ntf->protocol &&
        info->mode == ntf->mode &&
        nci_adapter_info_mode_params_matches(info, ntf) &&
        info->activation_param.size == ntf->activation_param_len &&
        (!ntf->activation_param_len || !memcmp(info->activation_param.bytes,
        ntf->activation_param_bytes, ntf->activation_param_len));
}

static
void
nci_adapter_drop_target(
    NciAdapter* self)
{
    NfcTarget* target = self->target;

    if (target) {
        NciAdapterPriv* priv = self->priv;

        self->target = NULL;
        priv->reactivating = FALSE;
        if (priv->presence_check_timer) {
            g_source_remove(priv->presence_check_timer);
            priv->presence_check_timer = 0;
        }
        if (priv->presence_check_id) {
            nfc_target_cancel_transmit(target, priv->presence_check_id);
            priv->presence_check_id = 0;
        }
        if (priv->active_intf) {
            g_free(priv->active_intf->mode_param_parsed);
            g_free(priv->active_intf);
            priv->active_intf = NULL;
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
        nci_adapter_deactivate(self, target);
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
    dest->prot_info = src->prot_info;
    memcpy(dest->app_data, src->app_data, sizeof(src->app_data));
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
    dest->t0 = src->t0;
    dest->ta = src->ta;
    dest->tb = src->tb;
    dest->tc = src->tc;
    return dest;
}

static
const NfcParamIsoDepPollB*
nci_adapter_convert_iso_dep_poll_b(
    NfcParamIsoDepPollB* dest,
    const NciActivationParamIsoDepPollB* src)
{
    dest->mbli = src->mbli;     /* Maximum buffer length index */
    dest->did = src->did;       /* Device ID */
    dest->hlr = src->hlr;       /* Higher Layer Response */
    return dest;
}

static
NfcTag*
nci_adapter_create_known_tag(
    NciAdapter* self,
    const NciIntfActivationNtf* ntf)
{
    const NciModeParam* mp = ntf->mode_param; /* Caller checked it for NULL */
    NfcParamPollA poll_a;
    NfcParamPollB poll_b;

    /* Figure out what kind of target we are dealing with */
    switch (ntf->mode) {
    case NCI_MODE_PASSIVE_POLL_A:
        switch (ntf->rf_intf) {
        case NCI_RF_INTERFACE_FRAME:
            /* Type 2 Tag */
            return nfc_adapter_add_tag_t2(NFC_ADAPTER(self), self->target,
                nci_adapter_convert_poll_a(&poll_a, &mp->poll_a));
        case NCI_RF_INTERFACE_ISO_DEP:
            /* ISO-DEP Type 4A */
            if (ntf->activation_param) {
                const NciActivationParam* ap = ntf->activation_param;
                NfcParamIsoDepPollA iso_dep_poll_a;

                return nfc_adapter_add_tag_t4a(NFC_ADAPTER(self),
                    self->target, nci_adapter_convert_poll_a
                        (&poll_a, &mp->poll_a),
                    nci_adapter_convert_iso_dep_poll_a
                        (&iso_dep_poll_a, &ap->iso_dep_poll_a));
            }
            break;
        case NCI_RF_INTERFACE_NFCEE_DIRECT:
        case NCI_RF_INTERFACE_NFC_DEP:
        case NCI_RF_INTERFACE_PROPRIETARY:
            break;
        }
        break;
    case NCI_MODE_PASSIVE_POLL_B:
        switch (ntf->rf_intf) {
        case NCI_RF_INTERFACE_ISO_DEP:
            /* ISO-DEP Type 4B */
            if (ntf->activation_param) {
                const NciActivationParam* ap = ntf->activation_param;
                NfcParamIsoDepPollB iso_dep_poll_b;

                return nfc_adapter_add_tag_t4b(NFC_ADAPTER(self),
                    self->target, nci_adapter_convert_poll_b
                        (&poll_b, &mp->poll_b),
                    nci_adapter_convert_iso_dep_poll_b
                        (&iso_dep_poll_b, &ap->iso_dep_poll_b));
            }
            break;
        case NCI_RF_INTERFACE_FRAME:
        case NCI_RF_INTERFACE_NFCEE_DIRECT:
        case NCI_RF_INTERFACE_NFC_DEP:
        case NCI_RF_INTERFACE_PROPRIETARY:
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
    return NULL;
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
    NfcTag* tag = NULL;
    NfcTarget* reactivated = NULL;

    if (!priv->reactivating) {
        /* Drop the previous target, if any */
        nci_adapter_drop_target(self);
    } else if (self->target &&
        !nci_adapter_intf_info_matches(priv->active_intf, ntf)) {
        GDEBUG("Different tag has arrived, dropping the old one");
        nci_adapter_drop_target(self);
    }

    if (self->target) {
        /* The same target has arrived */
        priv->reactivating = FALSE;
        reactivated = self->target;
    } else {
        /* Register the new tag */
        self->target = nci_target_new(self, ntf);
        priv->active_intf = nci_adapter_intf_info_new(ntf);
        if (ntf->mode_param) {
            tag = nci_adapter_create_known_tag(self, ntf);
        }
        if (!tag) {
            nfc_adapter_add_other_tag(NFC_ADAPTER(self), self->target);
        }
    }

    /* Start periodic presence checks */
    priv->presence_check_timer = g_timeout_add(PRESENCE_CHECK_PERIOD_MS,
        nci_adapter_presence_check_timer, self);

    /* Notify the core that target has beed reactivated */
    if (reactivated) {
        GDEBUG("Target reactivated");
        nfc_target_reactivated(reactivated);
    }
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

gboolean
nci_adapter_reactivate(
    NciAdapter* self,
    NfcTarget* target)
{
    if (self && self->target == target && target) {
        NciAdapterPriv* priv = self->priv;
        NciCore* nci = self->nci;

        if (priv->active_intf && !priv->reactivating && nci &&
            nci->current_state == NCI_RFST_POLL_ACTIVE &&
            nci->next_state == NCI_RFST_POLL_ACTIVE) {
            priv->reactivating = TRUE;
            if (priv->presence_check_timer) {
                /* Stop presence checks for the time being */
                g_source_remove(priv->presence_check_timer);
                priv->presence_check_timer = 0;
            }
            /* Switch to discovery and expect the same target to reappear */
            nci_core_set_state(nci, NCI_RFST_DISCOVERY);
            return TRUE;
        }
    }
    GWARN("Can't reactivate the tag in this state");
    return FALSE;
}

void
nci_adapter_deactivate(
    NciAdapter* self,
    NfcTarget* target)
{
    if (self && self->target == target && target) {
        nci_adapter_drop_target(self);
        if (self->parent.powered) {
            nci_core_set_state(self->nci, NCI_RFST_DISCOVERY);
        }
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
    NciAdapterPriv* priv = self->priv;
    NciCore* nci = self->nci;

    switch (nci->next_state) {
    case NCI_RFST_POLL_ACTIVE:
        break;
    case NCI_RFST_DISCOVERY:
    case NCI_RFST_W4_ALL_DISCOVERIES:
    case NCI_RFST_W4_HOST_SELECT:
        if (priv->reactivating) {
            /* Keep the target if we are waiting for it to reappear */
            break;
        }
        /* no break */
    default:
        nci_adapter_drop_target(self);
        break;
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
