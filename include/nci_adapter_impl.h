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
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#ifndef NCI_PLUGIN_H
#define NCI_PLUGIN_H

#include <nfc_adapter_impl.h>
#include <nci_plugin_types.h>

G_BEGIN_DECLS

/*
 * NciAdapter class implements mode switch methods of NfcAdapter,
 * the derived class is expected to implement power switch methods:
 * submit_power_request and cancel_power_request.
 */

typedef struct nci_adapter_priv NciAdapterPriv;

struct nci_adapter {
    NfcAdapter parent;
    NfcTarget* target;
    NciAdapterPriv* priv;
    NciCore* nci;
};

typedef struct nci_adapter_class {
    NfcAdapterClass parent;

    /* Derived class must invoke base implementation */
    void (*current_state_changed)(NciAdapter* adapter);
    void (*next_state_changed)(NciAdapter* adapter);

    /* Padding for future expansion */
    void (*_reserved1)(void);
    void (*_reserved2)(void);
    void (*_reserved3)(void);
    void (*_reserved4)(void);
    void (*_reserved5)(void);
    void (*_reserved6)(void);
    void (*_reserved7)(void);
    void (*_reserved8)(void);
    void (*_reserved9)(void);
    void (*_reserved10)(void);
} NciAdapterClass;

GType nci_adapter_get_type(void);
#define NCI_TYPE_ADAPTER (nci_adapter_get_type())
#define NCI_ADAPTER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        NCI_TYPE_ADAPTER, NciAdapter))
#define NCI_ADAPTER_CLASS(class) (G_TYPE_CHECK_CLASS_CAST((class), \
        NCI_TYPE_ADAPTER, NciAdapterClass))

void
nci_adapter_init_base(
    NciAdapter* adapter,
    NciHalIo* io);

/*
 * This can be called from finalize method of the derived class to make sure
 * that NciCore is freed before NciHalIo in cases when NciHalIo is allocated
 * dynamically.
 */
void
nci_adapter_finalize_core(
    NciAdapter* adapter);

G_END_DECLS

#endif /* NCI_PLUGIN_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
