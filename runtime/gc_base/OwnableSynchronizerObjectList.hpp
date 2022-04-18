
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

#ifndef OWNABLESYNCHRONIZEROBJECTLIST_HPP_
#define OWNABLESYNCHRONIZEROBJECTLIST_HPP_

#include "j9.h"
#include "j9cfg.h"
#include "j9nonbuilder.h"
#include "modron.h"

#include "BaseNonVirtual.hpp"
#include "ObjectAccessBarrier.hpp"

class MM_EnvironmentBase;

/**
 * A global list of OwnableSynchronizer objects.
 */
class MM_OwnableSynchronizerObjectList : public MM_BaseNonVirtual
{
/* data members */
private:
	bool				_needRefresh;
	volatile j9object_t _head; /**< the head of the linked list of OwnableSynchronizer objects */
	J9JavaVM 			*_javaVM;
	MM_GCExtensions 	*_extensions;
protected:
public:
	
/* function members */
private:
	MMINLINE bool isOwnableSynchronizerObject(j9object_t object)
	{
		bool ret = false;
		J9Class *objectClass =  J9GC_J9OBJECT_CLAZZ_VM(object, _javaVM);
		if ((OBJECT_HEADER_SHAPE_MIXED == J9GC_CLASS_SHAPE(objectClass)) && (J9CLASS_FLAGS(objectClass) & J9AccClassOwnableSynchronizer)) {
			ret = true;
		}
		return ret;
	}

	void add(j9object_t object)
	{
		Assert_MM_true(NULL != object);

		j9object_t previousHead = _head;
		while (previousHead != (j9object_t)MM_AtomicOperations::lockCompareExchange((volatile UDATA*)&_head, (UDATA)previousHead, (UDATA)object)) {
			previousHead = _head;
		}

		/* detect trivial cases which can inject cycles into the linked list */
		Assert_MM_true(_head != previousHead);

		_extensions->accessBarrier->setOwnableSynchronizerLink(object, previousHead);
	}

protected:
public:

	static MM_OwnableSynchronizerObjectList* newInstance(MM_EnvironmentBase *env, MM_GCExtensions* extensions);
	virtual void kill(MM_EnvironmentBase* env);

	/**
	 * check if an object, which was encountered directly while walking the heap, is ownableSynchronizerObject,
	 * and if the object is ownableSynchronizerObject, add it in OwnableSynchronizerObjectList.
	 *
	 * @param objectDesc the object
	 * @regionDesc the region which contains the object
	 *
	 * @return #J9MODRON_SLOT_ITERATOR_OK on success, #J9MODRON_SLOT_ITERATOR_UNRECOVERABLE_ERROR on failure
	 */
	uintptr_t walkObjectHeap(J9JavaVM *javaVM, J9MM_IterateObjectDescriptor *objectDesc, J9MM_IterateRegionDescriptor *regionDesc);

	/**
	 * Fetch the head of the linked list, as it appeared at the beginning of OwnableSynchronizer object processing.
	 * @return the head object, or NULL if the list is empty
	 */
	MMINLINE j9object_t getHeadOfList(MM_EnvironmentBase *env) {
		if (_needRefresh) {
			rebuildList(env);
			_needRefresh = false;
		}
		return _head;
	}

	void reset() {
		_needRefresh = true;
		_head = NULL; 
	}
	
	void ensureHeapWalkable(MM_EnvironmentBase *env);
	/*
	 * rebuildList need to run under ExclusiveVMAccess and no ConcurrentScavengerInProgress
	 */
	void rebuildList(MM_EnvironmentBase *env);
	/**
	 * Construct a new list.
	 */
	MM_OwnableSynchronizerObjectList(MM_GCExtensions* extensions)
		: MM_BaseNonVirtual()
		, _needRefresh(true)
		, _head(NULL)
		, _javaVM(NULL)
		, _extensions(extensions)
	{
		_typeId = __FUNCTION__;
	}
};

#endif /* OWNABLESYNCHRONIZEROBJECTLIST_HPP_ */
