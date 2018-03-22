/*
 * Copyright (c) 2001, 2005, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
/*
 * This module tracks classes that have been prepared, so as to
 * be able to compute which have been unloaded.  On VM start-up
 * all prepared classes are put in a table.  As class prepare
 * events come in they are added to the table.  After an unload
 * event or series of them, the VM can be asked for the list
 * of classes; this list is compared against the table keep by
 * this module, any classes no longer present are known to
 * have been unloaded.
 *
 * ANDROID-CHANGED: This module is almost totally re-written
 * for android. On android, we have a limited number of jweak
 * references that can be around at any one time. In order to
 * preserve this limited resource for user-code use we keep
 * track of the status of classes using JVMTI tags.
 *
 * We keep a linked-list of the signatures of loaded classes
 * associated with the tag we gave to that class. The tag is
 * simply incremented every time we add a new class.
 *
 * When we compare with the previous set of classes we iterate
 * through the list and remove any nodes which have no objects
 * associated with their tag, reporting these as the classes
 * that have been unloaded.
 *
 * For efficiency and simplicity we don't bother retagging or
 * re-using old tags, instead relying on the fact that no
 * program will ever be able to exhaust the (2^64 - 1) possible
 * tag values (which would require that many class-loads).
 *
 * This relies on the tagging implementation being relatively
 * efficient for performance. It has the advantage of not
 * requiring any jweaks.
 *
 * All calls into any function of this module must be either
 * done before the event-handler system is setup or done while
 * holding the event handlerLock.
 */

#include "util.h"
#include "bag.h"
#include "classTrack.h"

typedef struct KlassNode {
    jlong klass_tag;         /* Tag the klass has in the tracking-env */
    char *signature;         /* class signature */
    struct KlassNode *next;  /* next node in this slot */
} KlassNode;

/*
 * pointer to first node of a linked list of prepared classes KlassNodes.
 */
static KlassNode *list;

/*
 * The JVMTI env we use to keep track of klass tags which allows us to detect class-unloads.
 */
static jvmtiEnv *trackingEnv;

/*
 * The current highest tag number in use by the trackingEnv.
 *
 * No need for synchronization since everything is done under the handlerLock.
 */
static jlong currentKlassTag;

/*
 * Delete a linked-list of classes.
 * The signatures of classes in the table are returned.
 */
static struct bag *
deleteList(KlassNode *node)
{
    struct bag *signatures = bagCreateBag(sizeof(char*), 10);
    jint slot;

    if (signatures == NULL) {
        EXIT_ERROR(AGENT_ERROR_OUT_OF_MEMORY,"signatures");
    }

    while (node != NULL) {
        KlassNode *next;
        char **sigSpot;

        /* Add signature to the signature bag */
        sigSpot = bagAdd(signatures);
        if (sigSpot == NULL) {
            EXIT_ERROR(AGENT_ERROR_OUT_OF_MEMORY,"signature bag");
        }
        *sigSpot = node->signature;

        /* No need to delete the tag since the object was already destroyed. */
        next = node->next;
        jvmtiDeallocate(node);

        node = next;
    }

    return signatures;
}

static jboolean
isClassUnloaded(jlong tag) {
    jvmtiError error;
    jint res_count;
    error = JVMTI_FUNC_PTR(trackingEnv,GetObjectsWithTags)(trackingEnv,
                                                           /*tag_count*/ 1,
                                                           &tag,
                                                           &res_count,
                                                           /*object_result_ptr*/ NULL,
                                                           /*tag_result_ptr*/ NULL);
    if (error != JVMTI_ERROR_NONE) {
        EXIT_ERROR(error,"Failed GetObjectsWithTags for class tracking");
    }
    if (res_count != 0 && res_count != 1) {
        EXIT_ERROR(AGENT_ERROR_INTERNAL,"Unexpected extra tags in trackingEnv!");
    }
    return res_count == 0 ? JNI_TRUE : JNI_FALSE;
}

/*
 * Called after class unloads have occurred.  Creates a new hash table
 * of currently loaded prepared classes.
 * The signatures of classes which were unloaded (not present in the
 * new table) are returned.
 *
 * NB This relies on addPreparedClass being called for every class loaded after the
 * classTrack_initialize function is called. We will not request all loaded classes again after
 * that. It also relies on not being called concurrently with any classTrack_addPreparedClass or
 * other classTrack_processUnloads calls.
 */
struct bag *
classTrack_processUnloads(JNIEnv *env)
{
    KlassNode *toDeleteList = NULL;
    jboolean anyRemoved = JNI_FALSE;
    KlassNode* node = list;
    KlassNode** previousNext = &list;
    /* Filter out all the unloaded classes from the list. */
    while (node != NULL) {
        if (isClassUnloaded(node->klass_tag)) {
            /* Update the previous node's next pointer to point after this node. Note that we update
             * the value pointed to by previousNext but not the value of previousNext itself.
             */
            *previousNext = node->next;
            /* Remove this node from the 'list' and put it into toDeleteList */
            node->next = toDeleteList;
            toDeleteList = node;
            anyRemoved = JNI_TRUE;
        } else {
            /* This node will become the previous node so update the previousNext pointer to this
             * nodes next pointer.
             */
            previousNext = &(node->next);
        }
        node = *previousNext;
    }

    return deleteList(toDeleteList);
}

/*
 * Add a class to the prepared class list.
 * Assumes no duplicates.
 */
void
classTrack_addPreparedClass(JNIEnv *env, jclass klass)
{
    KlassNode *node;
    jvmtiError error;

    if (gdata->assertOn) {
        /* Check this is not a duplicate */
        jlong tag;
        error = JVMTI_FUNC_PTR(trackingEnv,GetTag)(trackingEnv, klass, &tag);
        if (error != JVMTI_ERROR_NONE) {
            EXIT_ERROR(error,"unable to get-tag with class trackingEnv!");
        }
        if (tag != 0l) {
            JDI_ASSERT_FAILED("Attempting to insert duplicate class");
        }
    }

    node = jvmtiAllocate(sizeof(KlassNode));
    if (node == NULL) {
        EXIT_ERROR(AGENT_ERROR_OUT_OF_MEMORY,"KlassNode");
    }
    error = classSignature(klass, &(node->signature), NULL);
    if (error != JVMTI_ERROR_NONE) {
        jvmtiDeallocate(node);
        EXIT_ERROR(error,"signature");
    }
    node->klass_tag = ++currentKlassTag;
    error = JVMTI_FUNC_PTR(trackingEnv,SetTag)(trackingEnv, klass, node->klass_tag);
    if (error != JVMTI_ERROR_NONE) {
        jvmtiDeallocate(node->signature);
        jvmtiDeallocate(node);
        EXIT_ERROR(error,"SetTag");
    }

    /* Insert the new node */
    node->next = list;
    list = node;
}

/*
 * Called once to build the initial prepared class hash table.
 */
void
classTrack_initialize(JNIEnv *env)
{
    /* ANDROID_CHANGED: Setup the tracking env and the currentKlassTag */
    trackingEnv = getSpecialJvmti();
    if ( trackingEnv == NULL ) {
        EXIT_ERROR(AGENT_ERROR_INTERNAL,"Failed to allocate tag-tracking jvmtiEnv");
    }
    currentKlassTag = 0l;
    list = NULL;
    WITH_LOCAL_REFS(env, 1) {

        jint classCount;
        jclass *classes;
        jvmtiError error;
        jint i;

        error = allLoadedClasses(&classes, &classCount);
        if ( error == JVMTI_ERROR_NONE ) {
            for (i=0; i<classCount; i++) {
                jclass klass = classes[i];
                jint status;
                jint wanted =
                    (JVMTI_CLASS_STATUS_PREPARED|JVMTI_CLASS_STATUS_ARRAY);

                /* We only want prepared classes and arrays */
                status = classStatus(klass);
                if ( (status & wanted) != 0 ) {
                    classTrack_addPreparedClass(env, klass);
                }
            }
            jvmtiDeallocate(classes);
        } else {
            EXIT_ERROR(error,"loaded classes array");
        }

    } END_WITH_LOCAL_REFS(env)

}

void
classTrack_reset(void)
{
}
