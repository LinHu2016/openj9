
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
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "j9.h"
#include "j9cfg.h"
#include "ModronAssertions.h"

#include "ContinuationObjectList.hpp"

#include "AtomicOperations.hpp"
#include "EnvironmentBase.hpp"
#include "GCExtensions.hpp"
#include "ObjectAccessBarrier.hpp"

MM_ContinuationObjectList::MM_ContinuationObjectList()
	: MM_BaseNonVirtual()
	, _head(NULL)
	, _priorHead(NULL)
	, _nextList(NULL)
	, _previousList(NULL)
//#if defined(J9VM_GC_VLHGC)
	, _objectCount(0)
//#endif /* defined(J9VM_GC_VLHGC) */
{
	_typeId = __FUNCTION__;
}

void
MM_ContinuationObjectList::addAll(MM_EnvironmentBase* env, j9object_t head, j9object_t tail)
{
	Assert_MM_true(NULL != head);
	Assert_MM_true(NULL != tail);

	j9object_t previousHead = _head;

	if ((head == previousHead) && (tail == previousHead))
	{
		PORT_ACCESS_FROM_ENVIRONMENT(env);
		j9tty_printf(PORTLIB, "addAll list=%p, head=%p, tail=%p, previousHead=%p", this, head, tail, previousHead);
	}

	while (previousHead != (j9object_t)MM_AtomicOperations::lockCompareExchange((volatile uintptr_t*)&_head, (uintptr_t)previousHead, (uintptr_t)head)) {
		previousHead = _head;
	}

	/* detect trivial cases which can inject cycles into the linked list */
//	Assert_MM_true( (head != previousHead) && (tail != previousHead) );
	Assert_GC_true_with_message4(env, (head != previousHead) && (tail != previousHead), "addAll list=%p, head=%p, tail=%p, previousHead=%p", this, head, tail, previousHead);

	MM_GCExtensions *extensions = MM_GCExtensions::getExtensions(env);
	extensions->accessBarrier->setContinuationLink(tail, previousHead);
}

void
MM_ContinuationObjectList::checkCircularList(MM_EnvironmentBase* env, bool start)
{
	MM_GCExtensions *extensions = MM_GCExtensions::getExtensions(env);
	j9object_t object = _head;
	if (start) {
		object = _priorHead;
	}
	intptr_t countInList = (intptr_t) getObjectCount();
	intptr_t count = countInList;
	PORT_ACCESS_FROM_ENVIRONMENT(env);
	j9tty_printf(PORTLIB, "checkCircularList list=%p, countInList=%zu, _head=%p, _priorHead=%p\n", this, countInList, _head, _priorHead);
	while ((object != NULL) && (count>=0)) {
		count--;
		object = extensions->accessBarrier->getContinuationLink(object);
	}

	if (start) {
		if (count!=0) {
			j9tty_printf(PORTLIB, "checkCircularList error env=%p, countInList=%d, count=%d\n", env, countInList, count);
		}
//		Assert_GC_true_with_message2(env, count==0, "countInList=%d, count=%i\n", countInList, count);
	} else {
		Assert_GC_true_with_message2(env, count>=0, "countInList=%d, count=%d\n", countInList, count);
	}
}
