/*
 * virsecretobj.c: internal <secret> objects handling
 *
 * Copyright (C) 2009-2016 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "datatypes.h"
#include "virsecretobj.h"
#include "viralloc.h"
#include "virerror.h"
#include "virfile.h"
#include "virhash.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_SECRET

VIR_LOG_INIT("conf.virsecretobj");

static virClassPtr virSecretObjClass;
static virClassPtr virSecretObjListClass;
static void virSecretObjDispose(void *obj);
static void virSecretObjListDispose(void *obj);

struct _virSecretObjList {
    virObjectLockable parent;

    /* uuid string -> virSecretObj  mapping
     * for O(1), lockless lookup-by-uuid */
    virHashTable *objs;
};

struct virSecretSearchData {
    int usageType;
    const char *usageID;
};


static int
virSecretObjOnceInit(void)
{
    if (!(virSecretObjClass = virClassNew(virClassForObjectLockable(),
                                          "virSecretObj",
                                          sizeof(virSecretObj),
                                          virSecretObjDispose)))
        return -1;

    if (!(virSecretObjListClass = virClassNew(virClassForObjectLockable(),
                                              "virSecretObjList",
                                              sizeof(virSecretObjList),
                                              virSecretObjListDispose)))
        return -1;

    return 0;
}


VIR_ONCE_GLOBAL_INIT(virSecretObj)

virSecretObjPtr
virSecretObjNew(void)
{
    virSecretObjPtr secret;

    if (virSecretObjInitialize() < 0)
        return NULL;

    if (!(secret = virObjectLockableNew(virSecretObjClass)))
        return NULL;

    return secret;
}


void
virSecretObjEndAPI(virSecretObjPtr *secret)
{
    if (!*secret)
        return;

    virObjectUnlock(*secret);
    virObjectUnref(*secret);
    *secret = NULL;
}


virSecretObjListPtr
virSecretObjListNew(void)
{
    virSecretObjListPtr secrets;

    if (virSecretObjInitialize() < 0)
        return NULL;

    if (!(secrets = virObjectLockableNew(virSecretObjListClass)))
        return NULL;

    if (!(secrets->objs = virHashCreate(50, virObjectFreeHashData))) {
        virObjectUnref(secrets);
        return NULL;
    }

    return secrets;
}


static void
virSecretObjDispose(void *obj)
{
    virSecretObjPtr secret = obj;

    virSecretDefFree(secret->def);
    if (secret->value) {
        /* Wipe before free to ensure we don't leave a secret on the heap */
        memset(secret->value, 0, secret->value_size);
        VIR_FREE(secret->value);
    }
    VIR_FREE(secret->configFile);
    VIR_FREE(secret->base64File);
}


static void
virSecretObjListDispose(void *obj)
{
    virSecretObjListPtr secrets = obj;

    virHashFree(secrets->objs);
}


/**
 * virSecretObjFindByUUIDLocked:
 * @secrets: list of secret objects
 * @uuid: secret uuid to find
 *
 * This functions requires @secrets to be locked already!
 *
 * Returns: not locked, but ref'd secret object.
 */
virSecretObjPtr
virSecretObjListFindByUUIDLocked(virSecretObjListPtr secrets,
                                 const unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(uuid, uuidstr);

    return virObjectRef(virHashLookup(secrets->objs, uuidstr));
}


/**
 * virSecretObjFindByUUID:
 * @secrets: list of secret objects
 * @uuid: secret uuid to find
 *
 * This function locks @secrets and finds the secret object which
 * corresponds to @uuid.
 *
 * Returns: locked and ref'd secret object.
 */
virSecretObjPtr
virSecretObjListFindByUUID(virSecretObjListPtr secrets,
                           const unsigned char *uuid)
{
    virSecretObjPtr ret;

    virObjectLock(secrets);
    ret = virSecretObjListFindByUUIDLocked(secrets, uuid);
    virObjectUnlock(secrets);
    if (ret)
        virObjectLock(ret);
    return ret;
}


static int
virSecretObjSearchName(const void *payload,
                       const void *name ATTRIBUTE_UNUSED,
                       const void *opaque)
{
    virSecretObjPtr secret = (virSecretObjPtr) payload;
    struct virSecretSearchData *data = (struct virSecretSearchData *) opaque;
    int found = 0;

    virObjectLock(secret);

    if (secret->def->usage_type != data->usageType)
        goto cleanup;

    switch (data->usageType) {
    case VIR_SECRET_USAGE_TYPE_NONE:
    /* never match this */
        break;

    case VIR_SECRET_USAGE_TYPE_VOLUME:
        if (STREQ(secret->def->usage.volume, data->usageID))
            found = 1;
        break;

    case VIR_SECRET_USAGE_TYPE_CEPH:
        if (STREQ(secret->def->usage.ceph, data->usageID))
            found = 1;
        break;

    case VIR_SECRET_USAGE_TYPE_ISCSI:
        if (STREQ(secret->def->usage.target, data->usageID))
            found = 1;
        break;
    }

 cleanup:
    virObjectUnlock(secret);
    return found;
}


/**
 * virSecretObjFindByUsageLocked:
 * @secrets: list of secret objects
 * @usageType: secret usageType to find
 * @usageID: secret usage string
 *
 * This functions requires @secrets to be locked already!
 *
 * Returns: not locked, but ref'd secret object.
 */
virSecretObjPtr
virSecretObjListFindByUsageLocked(virSecretObjListPtr secrets,
                                  int usageType,
                                  const char *usageID)
{
    virSecretObjPtr ret = NULL;
    struct virSecretSearchData data = { .usageType = usageType,
                                        .usageID = usageID };

    ret = virHashSearch(secrets->objs, virSecretObjSearchName, &data);
    if (ret)
        virObjectRef(ret);
    return ret;
}


/**
 * virSecretObjFindByUsage:
 * @secrets: list of secret objects
 * @usageType: secret usageType to find
 * @usageID: secret usage string
 *
 * This function locks @secrets and finds the secret object which
 * corresponds to @usageID of @usageType.
 *
 * Returns: locked and ref'd secret object.
 */
virSecretObjPtr
virSecretObjListFindByUsage(virSecretObjListPtr secrets,
                            int usageType,
                            const char *usageID)
{
    virSecretObjPtr ret;

    virObjectLock(secrets);
    ret = virSecretObjListFindByUsageLocked(secrets, usageType, usageID);
    virObjectUnlock(secrets);
    if (ret)
        virObjectLock(ret);
    return ret;
}


/*
 * virSecretObjListRemove:
 * @secrets: list of secret objects
 * @secret: a secret object
 *
 * Remove the object from the hash table.  The caller must hold the lock
 * on the driver owning @secrets and must have also locked @secret to
 * ensure no one else is either waiting for @secret or still using it.
 */
void
virSecretObjListRemove(virSecretObjListPtr secrets,
                       virSecretObjPtr secret)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(secret->def->uuid, uuidstr);
    virObjectRef(secret);
    virObjectUnlock(secret);

    virObjectLock(secrets);
    virObjectLock(secret);
    virHashRemoveEntry(secrets->objs, uuidstr);
    virObjectUnlock(secret);
    virObjectUnref(secret);
    virObjectUnlock(secrets);
}


/*
 * virSecretObjListAddLocked:
 * @secrets: list of secret objects
 * @def: new secret definition
 * @configDir: directory to place secret config files
 * @oldDef: Former secret def (e.g. a reload path perhaps)
 *
 * Add the new def to the secret obj table hash
 *
 * This functions requires @secrets to be locked already!
 *
 * Returns pointer to secret or NULL if failure to add
 */
virSecretObjPtr
virSecretObjListAddLocked(virSecretObjListPtr secrets,
                          virSecretDefPtr def,
                          const char *configDir,
                          virSecretDefPtr *oldDef)
{
    virSecretObjPtr secret;
    virSecretObjPtr ret = NULL;
    const char *newUsageID = virSecretUsageIDForDef(def);
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    char *configFile = NULL, *base64File = NULL;

    if (oldDef)
        *oldDef = NULL;

    /* Is there a secret already matching this UUID */
    if ((secret = virSecretObjListFindByUUIDLocked(secrets, def->uuid))) {
        const char *oldUsageID;

        virObjectLock(secret);

        oldUsageID = virSecretUsageIDForDef(secret->def);
        if (STRNEQ(oldUsageID, newUsageID)) {
            virUUIDFormat(secret->def->uuid, uuidstr);
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("a secret with UUID %s is already defined for "
                             "use with %s"),
                           uuidstr, oldUsageID);
            goto cleanup;
        }

        if (secret->def->private && !def->private) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("cannot change private flag on existing secret"));
            goto cleanup;
        }

        if (oldDef)
            *oldDef = secret->def;
        else
            virSecretDefFree(secret->def);
        secret->def = def;
    } else {
        /* No existing secret with same UUID,
         * try look for matching usage instead */
        if ((secret = virSecretObjListFindByUsageLocked(secrets,
                                                        def->usage_type,
                                                        newUsageID))) {
            virObjectLock(secret);
            virUUIDFormat(secret->def->uuid, uuidstr);
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("a secret with UUID %s already defined for "
                             "use with %s"),
                           uuidstr, newUsageID);
            goto cleanup;
        }

        /* Generate the possible configFile and base64File strings
         * using the configDir, uuidstr, and appropriate suffix
         */
        virUUIDFormat(def->uuid, uuidstr);
        if (!(configFile = virFileBuildPath(configDir, uuidstr, ".xml")) ||
            !(base64File = virFileBuildPath(configDir, uuidstr, ".base64")))
            goto cleanup;

        if (!(secret = virSecretObjNew()))
            goto cleanup;

        virObjectLock(secret);

        if (virHashAddEntry(secrets->objs, uuidstr, secret) < 0)
            goto cleanup;

        secret->def = def;
        secret->configFile = configFile;
        secret->base64File = base64File;
        configFile = NULL;
        base64File = NULL;
        virObjectRef(secret);
    }

    ret = secret;
    secret = NULL;

 cleanup:
    virSecretObjEndAPI(&secret);
    VIR_FREE(configFile);
    VIR_FREE(base64File);
    return ret;
}


virSecretObjPtr
virSecretObjListAdd(virSecretObjListPtr secrets,
                    virSecretDefPtr def,
                    const char *configDir,
                    virSecretDefPtr *oldDef)
{
    virSecretObjPtr ret;

    virObjectLock(secrets);
    ret = virSecretObjListAddLocked(secrets, def, configDir, oldDef);
    virObjectUnlock(secrets);
    return ret;
}


struct virSecretObjListGetHelperData {
    virConnectPtr conn;
    virSecretObjListACLFilter filter;
    int got;
    char **uuids;
    int nuuids;
    bool error;
};


static int
virSecretObjListGetHelper(void *payload,
                          const void *name ATTRIBUTE_UNUSED,
                          void *opaque)
{
    struct virSecretObjListGetHelperData *data = opaque;
    virSecretObjPtr obj = payload;

    if (data->error)
        return 0;

    if (data->nuuids >= 0 && data->got == data->nuuids)
        return 0;

    virObjectLock(obj);

    if (data->filter && !data->filter(data->conn, obj->def))
        goto cleanup;

    if (data->uuids) {
        char *uuidstr;

        if (VIR_ALLOC_N(uuidstr, VIR_UUID_STRING_BUFLEN) < 0)
            goto cleanup;

        virUUIDFormat(obj->def->uuid, uuidstr);
        data->uuids[data->got] = uuidstr;
    }

    data->got++;

 cleanup:
    virObjectUnlock(obj);
    return 0;
}


int
virSecretObjListNumOfSecrets(virSecretObjListPtr secrets,
                             virSecretObjListACLFilter filter,
                             virConnectPtr conn)
{
    struct virSecretObjListGetHelperData data = {
        .conn = conn, .filter = filter, .got = 0,
        .uuids = NULL, .nuuids = -1, .error = false };

    virObjectLock(secrets);
    virHashForEach(secrets->objs, virSecretObjListGetHelper, &data);
    virObjectUnlock(secrets);

    return data.got;
}


#define MATCH(FLAG) (flags & (FLAG))
static bool
virSecretObjMatchFlags(virSecretObjPtr secret,
                       unsigned int flags)
{
    /* filter by whether it's ephemeral */
    if (MATCH(VIR_CONNECT_LIST_SECRETS_FILTERS_EPHEMERAL) &&
        !((MATCH(VIR_CONNECT_LIST_SECRETS_EPHEMERAL) &&
           secret->def->ephemeral) ||
          (MATCH(VIR_CONNECT_LIST_SECRETS_NO_EPHEMERAL) &&
           !secret->def->ephemeral)))
        return false;

    /* filter by whether it's private */
    if (MATCH(VIR_CONNECT_LIST_SECRETS_FILTERS_PRIVATE) &&
        !((MATCH(VIR_CONNECT_LIST_SECRETS_PRIVATE) &&
           secret->def->private) ||
          (MATCH(VIR_CONNECT_LIST_SECRETS_NO_PRIVATE) &&
           !secret->def->private)))
        return false;

    return true;
}
#undef MATCH


struct virSecretObjListData {
    virConnectPtr conn;
    virSecretPtr *secrets;
    virSecretObjListACLFilter filter;
    unsigned int flags;
    int nsecrets;
    bool error;
};

static int
virSecretObjListPopulate(void *payload,
                         const void *name ATTRIBUTE_UNUSED,
                         void *opaque)
{
    struct virSecretObjListData *data = opaque;
    virSecretObjPtr obj = payload;
    virSecretPtr secret = NULL;

    if (data->error)
        return 0;

    virObjectLock(obj);

    if (data->filter && !data->filter(data->conn, obj->def))
        goto cleanup;

    if (!virSecretObjMatchFlags(obj, data->flags))
        goto cleanup;

    if (!data->secrets) {
        data->nsecrets++;
        goto cleanup;
    }

    if (!(secret = virGetSecret(data->conn, obj->def->uuid,
                                obj->def->usage_type,
                                virSecretUsageIDForDef(obj->def)))) {
        data->error = true;
        goto cleanup;
    }

    data->secrets[data->nsecrets++] = secret;

 cleanup:
    virObjectUnlock(obj);
    return 0;
}


int
virSecretObjListExport(virConnectPtr conn,
                       virSecretObjListPtr secretobjs,
                       virSecretPtr **secrets,
                       virSecretObjListACLFilter filter,
                       unsigned int flags)
{
    int ret = -1;
    struct virSecretObjListData data = {
        .conn = conn, .secrets = NULL,
        .filter = filter, .flags = flags,
        .nsecrets = 0, .error = false };

    virObjectLock(secretobjs);
    if (secrets &&
        VIR_ALLOC_N(data.secrets, virHashSize(secretobjs->objs) + 1) < 0)
        goto cleanup;

    virHashForEach(secretobjs->objs, virSecretObjListPopulate, &data);

    if (data.error)
        goto cleanup;

    if (data.secrets) {
        /* trim the array to the final size */
        ignore_value(VIR_REALLOC_N(data.secrets, data.nsecrets + 1));
        *secrets = data.secrets;
        data.secrets = NULL;
    }

    ret = data.nsecrets;

 cleanup:
    virObjectUnlock(secretobjs);
    while (data.secrets && data.nsecrets)
        virObjectUnref(data.secrets[--data.nsecrets]);

    VIR_FREE(data.secrets);
    return ret;
}


int
virSecretObjListGetUUIDs(virSecretObjListPtr secrets,
                         char **uuids,
                         int nuuids,
                         virSecretObjListACLFilter filter,
                         virConnectPtr conn)
{
    int ret = -1;

    struct virSecretObjListGetHelperData data = {
        .conn = conn, .filter = filter, .got = 0,
        .uuids = uuids, .nuuids = nuuids, .error = false };

    virObjectLock(secrets);
    virHashForEach(secrets->objs, virSecretObjListGetHelper, &data);
    virObjectUnlock(secrets);

    if (data.error)
        goto cleanup;

    ret = data.got;

 cleanup:
    if (ret < 0) {
        while (data.got)
            VIR_FREE(data.uuids[--data.got]);
    }
    return ret;
}
