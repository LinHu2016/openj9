
/*******************************************************************************
 * Copyright (c) 1991, 2022 IBM Corp. and others
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
#include "modron.h"

#include "BaseNonVirtual.hpp"
#include "EnvironmentBase.hpp"

/**
 * A global list of OwnableSynchronizer objects.
 */
class MM_OwnableSynchronizerObjectList : public MM_BaseNonVirtual
{
/* data members */
private:
	volatile j9object_t _head; /**< the head of the linked list of OwnableSynchronizer objects */
	j9object_t _priorHead; /**< the head of the linked list before OwnableSynchronizer object processing */
	MM_OwnableSynchronizerObjectList *_nextList; /**< a pointer to the next OwnableSynchronizer list in the global linked list of lists */
	MM_OwnableSynchronizerObjectList *_previousList; /**< a pointer to the previous OwnableSynchronizer list in the global linked list of lists */
#if defined(J9VM_GC_VLHGC)
	UDATA _objectCount; /**< the number of objects in the list */
#endif /* defined(J9VM_GC_VLHGC) */
protected:
public:
	
/* function members */
private:
protected:
public:

	MMINLINE j9object_t getHeadOfList() { return _head; }
	MMINLINE void setHeadOfList(j9object_t head) { _head = head; }
	MMINLINE MM_OwnableSynchronizerObjectList* getNextList() { return _nextList; }
	MMINLINE void setNextList(MM_OwnableSynchronizerObjectList* nextList) { _nextList = nextList; }

	static MM_OwnableSynchronizerObjectList* newInstance(MM_EnvironmentBase *env)
	{
		MM_OwnableSynchronizerObjectList *ownableSynchronizerObjectList = (MM_OwnableSynchronizerObjectList *)env->getForge()->allocate(sizeof(MM_OwnableSynchronizerObjectList), MM_AllocationCategory::FIXED, J9_GET_CALLSITE());
		if (ownableSynchronizerObjectList) {
			new(ownableSynchronizerObjectList) MM_OwnableSynchronizerObjectList();
		}
		return ownableSynchronizerObjectList;
	}

	virtual void kill(MM_EnvironmentBase* env)
	{
		env->getForge()->free(this);
	}

	/**
	 * Construct a new list.
	 */
	MM_OwnableSynchronizerObjectList()
	: MM_BaseNonVirtual()
	, _head(NULL)
	, _priorHead(NULL)
	, _nextList(NULL)
	, _previousList(NULL)
#if defined(J9VM_GC_VLHGC)
	, _objectCount(0)
#endif /* defined(J9VM_GC_VLHGC) */
	{
		_typeId = __FUNCTION__;
	}
};

#endif /* OWNABLESYNCHRONIZEROBJECTLIST_HPP_ */
