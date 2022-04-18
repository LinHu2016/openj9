
/*******************************************************************************
 * Copyright (c) 2022, 2022 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "j9.h"
#include "j9cfg.h"
#include "ModronAssertions.h"

#include "HeapIteratorAPI.h"
#include "EnvironmentBase.hpp"

#include "OwnableSynchronizerObjectListOnTheFly.hpp"

/**
 * Return codes for iterator functions.
 * Verification continues if an iterator function returns J9MODRON_SLOT_ITERATOR_OK,
 * otherwise it terminates.
 * @name Iterator return codes
 * @{
 */
#define J9MODRON_SLOT_ITERATOR_OK 	((UDATA)0x00000000) /**< Indicates success */
#define J9MODRON_SLOT_ITERATOR_UNRECOVERABLE_ERROR	((UDATA)0x00000001) /**< Indicates that an unrecoverable error was detected */
#define J9MODRON_SLOT_ITERATOR_RECOVERABLE_ERROR ((UDATA)0x00000002) /** < Indicates that a recoverable error was detected */

typedef struct ObjectIteratorCallbackUserData1 {
	MM_OwnableSynchronizerObjectListOnTheFly* ownableSynchronizerObjectListOnTheFly;
	J9PortLibrary* portLibrary; /* Input */
	J9MM_IterateRegionDescriptor* regionDesc; /* Temp - used internally by iterator functions */
} ObjectIteratorCallbackUserData1;

/**
 * Iterator callbacks, these are chained to eventually get to objects and their regions.
 */
static jvmtiIterationControl walk_heapIteratorCallback(J9JavaVM* vm, J9MM_IterateHeapDescriptor* heapDesc, void* userData);
static jvmtiIterationControl walk_spaceIteratorCallback(J9JavaVM* vm, J9MM_IterateSpaceDescriptor* spaceDesc, void* userData);
static jvmtiIterationControl walk_regionIteratorCallback(J9JavaVM* vm, J9MM_IterateRegionDescriptor* regionDesc, void* userData);
static jvmtiIterationControl walk_objectIteratorCallback(J9JavaVM* vm, J9MM_IterateObjectDescriptor* objectDesc, void* userData);

MM_OwnableSynchronizerObjectListOnTheFly*
MM_OwnableSynchronizerObjectListOnTheFly::newInstance(MM_EnvironmentBase *env, MM_GCExtensions* extensions)
{
	MM_OwnableSynchronizerObjectListOnTheFly *ownableSynchronizerObjectList = (MM_OwnableSynchronizerObjectListOnTheFly *)env->getForge()->allocate(sizeof(MM_OwnableSynchronizerObjectListOnTheFly), MM_AllocationCategory::FIXED, J9_GET_CALLSITE());
	if (ownableSynchronizerObjectList) {
		new(ownableSynchronizerObjectList) MM_OwnableSynchronizerObjectListOnTheFly(extensions);
	}
	return ownableSynchronizerObjectList;
}

void
MM_OwnableSynchronizerObjectListOnTheFly::kill(MM_EnvironmentBase* env)
{
	env->getForge()->free(this);
}

uintptr_t
MM_OwnableSynchronizerObjectListOnTheFly::walkObjectHeap(J9JavaVM *javaVM, J9MM_IterateObjectDescriptor *objectDesc, J9MM_IterateRegionDescriptor *regionDesc)
{
	uintptr_t result = J9MODRON_SLOT_ITERATOR_OK;
	if (TRUE == objectDesc->isObject) {
		if (isOwnableSynchronizerObject(objectDesc->object)) {
			add(objectDesc->object);
		}
	}

	return result;
}

void 
MM_OwnableSynchronizerObjectListOnTheFly::rebuildList()
{
	if (NULL == _javaVM) {
		_javaVM = _extensions->getJavaVM();
	}

	ObjectIteratorCallbackUserData1 userData;
	userData.ownableSynchronizerObjectListOnTheFly = this;
	userData.portLibrary = _javaVM->portLibrary;
	userData.regionDesc = NULL;
	_javaVM->memoryManagerFunctions->j9mm_iterate_heaps(_javaVM, _javaVM->portLibrary, 0, walk_heapIteratorCallback, &userData);
}

static jvmtiIterationControl
walk_heapIteratorCallback(J9JavaVM* vm, J9MM_IterateHeapDescriptor* heapDesc, void* userData)
{
	ObjectIteratorCallbackUserData1* castUserData = (ObjectIteratorCallbackUserData1*)userData;
	vm->memoryManagerFunctions->j9mm_iterate_spaces(vm, castUserData->portLibrary, heapDesc, 0, walk_spaceIteratorCallback, castUserData);
	return JVMTI_ITERATION_CONTINUE;
}

static jvmtiIterationControl
walk_spaceIteratorCallback(J9JavaVM* vm, J9MM_IterateSpaceDescriptor* spaceDesc, void* userData)
{
	ObjectIteratorCallbackUserData1* castUserData = (ObjectIteratorCallbackUserData1*)userData;
	vm->memoryManagerFunctions->j9mm_iterate_regions(vm, castUserData->portLibrary, spaceDesc, 0, walk_regionIteratorCallback, castUserData);
	return JVMTI_ITERATION_CONTINUE;
}

static jvmtiIterationControl
walk_regionIteratorCallback(J9JavaVM* vm, J9MM_IterateRegionDescriptor* regionDesc, void* userData)
{
	ObjectIteratorCallbackUserData1* castUserData = (ObjectIteratorCallbackUserData1*)userData;
	castUserData->regionDesc = regionDesc;
	vm->memoryManagerFunctions->j9mm_iterate_region_objects(vm, castUserData->portLibrary, regionDesc, j9mm_iterator_flag_include_holes, walk_objectIteratorCallback, castUserData);
	return JVMTI_ITERATION_CONTINUE;
}

static jvmtiIterationControl
walk_objectIteratorCallback(J9JavaVM* vm, J9MM_IterateObjectDescriptor* objectDesc, void* userData)
{
	ObjectIteratorCallbackUserData1* castUserData = (ObjectIteratorCallbackUserData1*)userData;
	if (castUserData->ownableSynchronizerObjectListOnTheFly->walkObjectHeap(vm, objectDesc, castUserData->regionDesc) != J9MODRON_SLOT_ITERATOR_OK) {
		return JVMTI_ITERATION_ABORT;
	}
	return JVMTI_ITERATION_CONTINUE;
}

