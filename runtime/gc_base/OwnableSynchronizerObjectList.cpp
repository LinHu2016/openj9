
/*******************************************************************************
 * Copyright IBM Corp. and others 1991
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
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "j9.h"
#include "j9cfg.h"
#include "j9nonbuilder.h"
#include "ModronAssertions.h"
#include "ObjectAccessBarrier.hpp"
#include "HeapIteratorAPI.h"

#include "OwnableSynchronizerObjectList.hpp"

#include "HeapRegionDescriptor.hpp"
#include "HeapRegionIterator.hpp"
#include "HeapMapIterator.hpp"
#include "EnvironmentBase.hpp"

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
	MM_OwnableSynchronizerObjectList* ownableSynchronizerObjectList;
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

MM_OwnableSynchronizerObjectList::MM_OwnableSynchronizerObjectList(MM_GCExtensions* extensions)
	: MM_BaseNonVirtual()
	, _needRefresh(true)
	, _head(NULL)
	, _nextList(NULL)
	, _javaVM(NULL)
	, _extensions(extensions)
{
	_typeId = __FUNCTION__;
}

MM_OwnableSynchronizerObjectList*
MM_OwnableSynchronizerObjectList::newInstance(MM_EnvironmentBase *env, MM_GCExtensions* extensions)
{
	MM_OwnableSynchronizerObjectList *ownableSynchronizerObjectList = (MM_OwnableSynchronizerObjectList *)env->getForge()->allocate(sizeof(MM_OwnableSynchronizerObjectList), MM_AllocationCategory::FIXED, J9_GET_CALLSITE());
	if (ownableSynchronizerObjectList) {
		new(ownableSynchronizerObjectList) MM_OwnableSynchronizerObjectList(extensions);
	}
	return ownableSynchronizerObjectList;
}

void
MM_OwnableSynchronizerObjectList::kill(MM_EnvironmentBase* env)
{
	env->getForge()->free(this);
}


/**
 * Determine whether the given object is an ownable synchronizer.
 *
 * An ownable synchronizer is a synchronization object that can be exclusively owned by a thread,
 * such as ReentrantLock, ReentrantReadWriteLock, or custom implementations extending
 * AbstractOwnableSynchronizer. These objects are tracked for thread monitoring and deadlock
 * detection via ThreadMXBean.
 *
 * @param object the object to check
 * @return true if the object is an ownable synchronizer, false otherwise
 *
 * @note The check verifies the conditions:
 *       1. The object has a mixed header shape (contains both object data and monitor fields)
 *       2. The object's class has the J9AccClassOwnableSynchronizer flag set
 *       3, no need to check if The object is not NULL and is valid because this function is only used during "heap walk"(heapWalkCallback())
 */

bool
MM_OwnableSynchronizerObjectList::isOwnableSynchronizerObject(j9object_t object)
{
	bool ret = false;
	J9Class *objectClass =  J9GC_J9OBJECT_CLAZZ_VM(object, _javaVM);
	if ((OBJECT_HEADER_SHAPE_MIXED == J9GC_CLASS_SHAPE(objectClass)) &&
		(J9AccClassOwnableSynchronizer == (J9CLASS_FLAGS(objectClass) & J9AccClassOwnableSynchronizer))) {
		ret = true;
	}
	return ret;
}

void
MM_OwnableSynchronizerObjectList::add(j9object_t object)
{
	Assert_MM_true(NULL != object);
	/* detect trivial cases which can inject cycles into the linked list */
	Assert_MM_true(_head != object);
	_extensions->accessBarrier->setOwnableSynchronizerLink(object, _head);
	_head = object;
}

uintptr_t
MM_OwnableSynchronizerObjectList::heapWalkCallback(J9JavaVM *javaVM, J9MM_IterateObjectDescriptor *objectDesc, J9MM_IterateRegionDescriptor *regionDesc)
{
	uintptr_t result = J9MODRON_SLOT_ITERATOR_OK;
	if (TRUE == objectDesc->isObject) {
		if (isOwnableSynchronizerObject(objectDesc->object)) {
			add(objectDesc->object);
		}
	}

	return result;
}

/**
 * Ensure the heap is in a walkable state by triggering a garbage collection.
 *
 * This method temporarily enables heap walking capabilities and performs a global GC
 * to compact and stabilize the heap before iteration.
 *
 * The method:
 * 1. Temporarily sets J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK if not already set
 * 2. Triggers a global GC with J9MMCONSTANT_EXPLICIT_GC_RASDUMP_COMPACT override
 * 3. For Metronome GC policy, triggers a second GC to ensure a complete cycle
 * 4. Restores the original heap walk attribute state
 *
 * @param env The current thread
 *
 * @note This method can be called while holding exclusive VM access
 * @note For Metronome GC, two GC cycles may be required to ensure completeness
 */
void
MM_OwnableSynchronizerObjectList::ensureHeapWalkable(MM_EnvironmentBase *env)
{

	if (NULL == _javaVM) {
		_javaVM = _extensions->getJavaVM();
	}

	J9VMThread *vmThread = (J9VMThread *)env->getLanguageVMThread();

//	uintptr_t savedGCFlags = _javaVM->requiredDebugAttributes & J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK;
//	if (savedGCFlags == 0) { /* if the flags was not set, you set it */
//		_javaVM->requiredDebugAttributes |= J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK;
//	}
	/* If heap walk is already enabled, nothing need be done */
	if (J9_ARE_NO_BITS_SET(_javaVM->requiredDebugAttributes, J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK)) {
		/* J9MMCONSTANT_EXPLICIT_GC_RASDUMP_COMPACT allows the GC to run while the current thread is holding
		 * exclusive VM access.
		 */
		_javaVM->requiredDebugAttributes |= J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK;

		PORT_ACCESS_FROM_ENVIRONMENT(env);
		j9tty_printf(PORTLIB, "MM_OwnableSynchronizerObjectList::ensureHeapWalkable VM_ACCESS=%zu, XACCESS_EXCLUSIVE=%zu\n",
				J9_ARE_ANY_BITS_SET(vmThread->publicFlags, J9_PUBLIC_FLAGS_VM_ACCESS),
				(J9_XACCESS_EXCLUSIVE == _javaVM->exclusiveAccessState));

		_javaVM->memoryManagerFunctions->j9gc_modron_global_collect_with_overrides(vmThread, J9MMCONSTANT_EXPLICIT_GC_RASDUMP_COMPACT);
		if (J9_GC_POLICY_METRONOME == _javaVM->gcPolicy) {
			/* In metronome, the previous GC call may have only finished the current cycle.
			 * Call again to ensure a full GC takes place.					 */
			j9tty_printf(PORTLIB, "MM_OwnableSynchronizerObjectList::ensureHeapWalkable j9gc_modron_global_collect_with_overrides second time\n");
			_javaVM->memoryManagerFunctions->j9gc_modron_global_collect_with_overrides(vmThread, J9MMCONSTANT_EXPLICIT_GC_RASDUMP_COMPACT);
		}
	}
//	if (savedGCFlags == 0) { /* if you set it, you have to unset it */
//		_javaVM->requiredDebugAttributes &= ~J9VM_DEBUG_ATTRIBUTE_ALLOW_USER_HEAP_WALK;
//	}
}

j9object_t
MM_OwnableSynchronizerObjectList::getHeadOfList(MM_EnvironmentBase *env)
{
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	j9tty_printf(PORTLIB, "MM_OwnableSynchronizerObjectList::getHeadOfList _head=%p\n", _head);

	if (_needRefresh) {
		rebuildList(env);
	}

	return _head;
}

void
MM_OwnableSynchronizerObjectList::rebuildList(MM_EnvironmentBase *env)
{
	if (NULL == _javaVM) {
		_javaVM = _extensions->getJavaVM();
	}

	PORT_ACCESS_FROM_ENVIRONMENT(env);
	j9tty_printf(PORTLIB, "MM_OwnableSynchronizerObjectList::rebuildList start \n");

	if ((NULL != env) && _extensions->requiresHeapWalkForRebuildingOwnableSynchronizerObjectList) {
		ensureHeapWalkable(env);
	}
	ObjectIteratorCallbackUserData1 userData;
	userData.ownableSynchronizerObjectList = this;
	userData.portLibrary = _javaVM->portLibrary;
	userData.regionDesc = NULL;
	_javaVM->memoryManagerFunctions->j9mm_iterate_heaps(_javaVM, _javaVM->portLibrary, 0, walk_heapIteratorCallback, &userData);
	_needRefresh = false;
	j9tty_printf(PORTLIB, "MM_OwnableSynchronizerObjectList::rebuildList end _needRefresh=%zu\n", _needRefresh);
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
	if (castUserData->ownableSynchronizerObjectList->heapWalkCallback(vm, objectDesc, castUserData->regionDesc) != J9MODRON_SLOT_ITERATOR_OK) {
		return JVMTI_ITERATION_ABORT;
	}
	return JVMTI_ITERATION_CONTINUE;
}
