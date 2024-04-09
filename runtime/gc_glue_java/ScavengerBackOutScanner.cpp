/*******************************************************************************
 * Copyright IBM Corp. and others 2015
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

#include "j9cfg.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "ConfigurationDelegate.hpp"
#include "FinalizableObjectBuffer.hpp"
#include "FinalizableReferenceBuffer.hpp"
#include "FinalizeListManager.hpp"
#include "ForwardedHeader.hpp"
#include "HeapRegionDescriptorStandard.hpp"
#include "HeapRegionIteratorStandard.hpp"
#include "ObjectAccessBarrier.hpp"
#include "ReferenceObjectBuffer.hpp"
#include "UnfinalizedObjectBuffer.hpp"
#include "ContinuationObjectBuffer.hpp"
#include "ContinuationObjectList.hpp"

#include "ScavengerBackOutScanner.hpp"

void
MM_ScavengerBackOutScanner::scanAllSlots(MM_EnvironmentBase *env)
{
	/* Clear the reference lists since the scavenge is being backed out */
	{
		GC_HeapRegionIteratorStandard regionIterator(_extensions->heapRegionManager);
		MM_HeapRegionDescriptorStandard *region = NULL;
		while (NULL != (region = regionIterator.nextRegion())) {
			if ((MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW))) {
				MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
				for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
					regionExtension->_referenceObjectLists[i].resetLists();
				}
			}
		}
	}

	/* Walk roots fixing up pointers through reverse forwarding information */
	MM_RootScanner::scanAllSlots(env);

	/* Back out Ownable Synchronizer and Continuation Processing */
	MM_HeapRegionDescriptorStandard *region = NULL;
	GC_HeapRegionIteratorStandard regionIterator(_extensions->heapRegionManager);

	PORT_ACCESS_FROM_ENVIRONMENT(env);
	j9tty_printf(PORTLIB, "MM_ScavengerBackOutScanner::scanAllSlots Back out Ownable Synchronizer and Continuation Processing start env=%p\n", env);

	while (NULL != (region = regionIterator.nextRegion())) {
		MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
		for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
			// Back out all of regions (includes tenure region, which is backed up during startProcessing).
			if (!_extensions->isConcurrentScavengerEnabled()) {
				regionExtension->_ownableSynchronizerObjectLists[i].backoutList();
			}
			if (!_extensions->isConcurrentScavengerEnabled() || (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW))) {
				if (_extensions->isConcurrentScavengerEnabled()) {
					// for concurrent scavenger case, only backout lists in Evacuate region( if the lists in Survivor region are empty, the backout would not hurt)
					omrobjectptr_t object = regionExtension->_continuationObjectLists[i].getHeadOfList();
					omrobjectptr_t priorObject = regionExtension->_continuationObjectLists[i].getPriorList();

					j9tty_printf(PORTLIB, "concurrent scavenger case env=%p, region=%p, i=%zu, list=%p, object=%p, priorObject=%p\n",
							env, region, i, &regionExtension->_continuationObjectLists[i], object, priorObject);

					if (NULL != object) {
						if (!_scavenger->isObjectInEvacuateMemory(object)) {
							j9tty_printf(PORTLIB, "keep Continuation survivorRegion env=%p, region=%p, lowAddr=%p, highAddr=%p, object=%p, priorObject=%p\n",
									env, region, region->getLowAddress(), region->getHighAddress(), object, priorObject);
							continue;
						} else if (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)) {
							j9tty_printf(PORTLIB, "backout Continuation evacurateRegion env=%p, region=%p, lowAddr=%p, highAddr=%p, object=%p, priorObject=%p\n",
									env, region, region->getLowAddress(), region->getHighAddress(), object, priorObject);
						} else {
							j9tty_printf(PORTLIB, "backout Continuation TenureRegion env=%p, region=%p, lowAddr=%p, highAddr=%p, object=%p, priorObject=%p\n",
									env, region, region->getLowAddress(), region->getHighAddress(), object, priorObject);
						}
					} else if (NULL != priorObject) {
						if (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)) {
							j9tty_printf(PORTLIB, "backout Continuation evacurateRegion env=%p, region=%p, lowAddr=%p, highAddr=%p, object=%p, priorObject=%p\n",
									env, region, region->getLowAddress(), region->getHighAddress(), object, priorObject);
						} else {
							j9tty_printf(PORTLIB, "backout Continuation TenureRegion env=%p, region=%p, lowAddr=%p, highAddr=%p, object=%p, priorObject=%p\n",
									env, region, region->getLowAddress(), region->getHighAddress(), object, priorObject);
						}
					}

				}
				regionExtension->_continuationObjectLists[i].backoutList();
			}
		}
	}

	j9tty_printf(PORTLIB, "MM_ScavengerBackOutScanner::scanAllSlots Back out Ownable Synchronizer and Continuation Processing end env=%p\n", env);
 	/* Done backout */
	Assert_MM_true(env->getGCEnvironment()->_referenceObjectBuffer->isEmpty());
}

#if defined(J9VM_GC_FINALIZATION)
void
MM_ScavengerBackOutScanner::backoutFinalizableObjects(MM_EnvironmentStandard *env)
{
	bool const compressed = _extensions->compressObjectReferences();
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	if (_extensions->isConcurrentScavengerEnabled()) {
		GC_FinalizeListManager * finalizeListManager = _extensions->finalizeListManager;
		{
			GC_FinalizableObjectBuffer objectBuffer(_extensions);
			/* walk finalizable objects loaded by the system class loader */
			omrobjectptr_t systemObject = finalizeListManager->resetSystemFinalizableObjects();
			while (NULL != systemObject) {
				MM_ForwardedHeader forwardHeader(systemObject, compressed);
				omrobjectptr_t forwardPtr = forwardHeader.getNonStrictForwardedObject();

				if (NULL != forwardPtr) {
					if (forwardHeader.isSelfForwardedPointer()) {
						forwardHeader.restoreSelfForwardedPointer();
					} else {
						systemObject = forwardPtr;
					}
				}

				omrobjectptr_t next = _extensions->accessBarrier->getFinalizeLink(systemObject);
				objectBuffer.add(env, systemObject);
				systemObject = next;
			}
			objectBuffer.flush(env);
		}

		{
			GC_FinalizableObjectBuffer objectBuffer(_extensions);
			/* walk finalizable objects loaded by the all other class loaders */
			omrobjectptr_t defaultObject = finalizeListManager->resetDefaultFinalizableObjects();
			while (NULL != defaultObject) {
				MM_ForwardedHeader forwardHeader(defaultObject, compressed);
				omrobjectptr_t forwardPtr = forwardHeader.getNonStrictForwardedObject();

				if (NULL != forwardPtr) {
					if (forwardHeader.isSelfForwardedPointer()) {
						forwardHeader.restoreSelfForwardedPointer();
					} else {
						defaultObject = forwardPtr;
					}
				}

				omrobjectptr_t next = _extensions->accessBarrier->getFinalizeLink(defaultObject);
				objectBuffer.add(env, defaultObject);
				defaultObject = next;
			}
			objectBuffer.flush(env);
		}

		{
			/* walk reference objects */
			GC_FinalizableReferenceBuffer referenceBuffer(_extensions);
			omrobjectptr_t referenceObject = finalizeListManager->resetReferenceObjects();
			while (NULL != referenceObject) {
				MM_ForwardedHeader forwardHeader(referenceObject, compressed);
				omrobjectptr_t forwardPtr = forwardHeader.getNonStrictForwardedObject();

				if (NULL != forwardPtr) {
					if (forwardHeader.isSelfForwardedPointer()) {
						forwardHeader.restoreSelfForwardedPointer();
					} else {
						referenceObject = forwardPtr;
					}
				}

				omrobjectptr_t next = _extensions->accessBarrier->getReferenceLink(referenceObject);
				referenceBuffer.add(env, referenceObject);
				referenceObject = next;
			}
			referenceBuffer.flush(env);
		}
	} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
	{
		GC_FinalizeListManager * finalizeListManager = _extensions->finalizeListManager;
		{
			GC_FinalizableObjectBuffer objectBuffer(_extensions);
			/* walk finalizable objects loaded by the system class loader */
			omrobjectptr_t systemObject = finalizeListManager->resetSystemFinalizableObjects();
			while (NULL != systemObject) {
				omrobjectptr_t next = NULL;
				MM_ForwardedHeader forwardHeader(systemObject, compressed);
				Assert_MM_false(forwardHeader.isForwardedPointer());
				if (forwardHeader.isReverseForwardedPointer()) {
					omrobjectptr_t originalObject = forwardHeader.getReverseForwardedPointer();
					Assert_MM_true(NULL != originalObject);
					/* Read the finalizeLink from the originalObject since it will be copied back in
					 * reverseForwardedObject and the current object may have had this slot destroyed by
					 * reverseForwardedObject.
					 */
					next = _extensions->accessBarrier->getFinalizeLink(originalObject);
					objectBuffer.add(env, originalObject);
				} else {
					next = _extensions->accessBarrier->getFinalizeLink(systemObject);
					objectBuffer.add(env, systemObject);
				}

				systemObject = next;
			}
			objectBuffer.flush(env);
		}

		{
			GC_FinalizableObjectBuffer objectBuffer(_extensions);
			/* walk finalizable objects loaded by the all other class loaders */
			omrobjectptr_t defaultObject = finalizeListManager->resetDefaultFinalizableObjects();
			while (NULL != defaultObject) {
				omrobjectptr_t next = NULL;
				MM_ForwardedHeader forwardHeader(defaultObject, compressed);
				Assert_MM_false(forwardHeader.isForwardedPointer());
				if (forwardHeader.isReverseForwardedPointer()) {
					omrobjectptr_t originalObject = forwardHeader.getReverseForwardedPointer();
					Assert_MM_true(NULL != originalObject);
					next = _extensions->accessBarrier->getFinalizeLink(originalObject);
					objectBuffer.add(env, originalObject);
				} else {
					next = _extensions->accessBarrier->getFinalizeLink(defaultObject);
					objectBuffer.add(env, defaultObject);
				}

				defaultObject = next;
			}
			objectBuffer.flush(env);
		}

		{
			/* walk reference objects */
			GC_FinalizableReferenceBuffer referenceBuffer(_extensions);
			omrobjectptr_t referenceObject = finalizeListManager->resetReferenceObjects();
			while (NULL != referenceObject) {
				omrobjectptr_t next = NULL;
				MM_ForwardedHeader forwardHeader(referenceObject, compressed);
				Assert_MM_false(forwardHeader.isForwardedPointer());
				if (forwardHeader.isReverseForwardedPointer()) {
					omrobjectptr_t originalObject = forwardHeader.getReverseForwardedPointer();
					Assert_MM_true(NULL != originalObject);

					next = _extensions->accessBarrier->getReferenceLink(originalObject);
					referenceBuffer.add(env, originalObject);
				} else {
					next = _extensions->accessBarrier->getReferenceLink(referenceObject);
					referenceBuffer.add(env, referenceObject);
				}

				referenceObject = next;
			}
			referenceBuffer.flush(env);
		}
	}
}

/**
 * Backout the unfinalized objects.  ALL regions must be iterated as some unfinalized objects
 * may have been tenured.
 *
 * @NOTE it is assumed that this code will be called while the GC is in single threaded backout
 */
void
MM_ScavengerBackOutScanner::backoutUnfinalizedObjects(MM_EnvironmentStandard *env)
{
	MM_Heap *heap = _extensions->heap;
	MM_HeapRegionManager *regionManager = heap->getHeapRegionManager();
	MM_HeapRegionDescriptorStandard *region = NULL;
	bool const compressed = _extensions->compressObjectReferences();

	GC_HeapRegionIteratorStandard regionIterator(regionManager);
	while (NULL != (region = regionIterator.nextRegion())) {
		MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
		for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
			MM_UnfinalizedObjectList *list = &regionExtension->_unfinalizedObjectLists[i];
			list->startUnfinalizedProcessing();
		}
	}

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	if (_extensions->isConcurrentScavengerEnabled()) {
		GC_HeapRegionIteratorStandard regionIterator2(regionManager);
		while (NULL != (region = regionIterator2.nextRegion())) {
			MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
			for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
				MM_UnfinalizedObjectList *list = &regionExtension->_unfinalizedObjectLists[i];
				if (!list->wasEmpty()) {
					omrobjectptr_t object = list->getPriorList();
					while (NULL != object) {
						MM_ForwardedHeader forwardHeader(object, compressed);
						omrobjectptr_t forwardPtr = forwardHeader.getNonStrictForwardedObject();
						if (NULL != forwardPtr) {
							if (forwardHeader.isSelfForwardedPointer()) {
								forwardHeader.restoreSelfForwardedPointer();
							} else {
								object = forwardPtr;
							}
						}

						omrobjectptr_t next = _extensions->accessBarrier->getFinalizeLink(object);
						env->getGCEnvironment()->_unfinalizedObjectBuffer->add(env, object);

						object = next;
					}
				}
			}
		}
	} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
	{
		GC_HeapRegionIteratorStandard regionIterator2(regionManager);
		while (NULL != (region = regionIterator2.nextRegion())) {
			MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
			for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
				MM_UnfinalizedObjectList *list = &regionExtension->_unfinalizedObjectLists[i];
				if (!list->wasEmpty()) {
					omrobjectptr_t object = list->getPriorList();
					while (NULL != object) {
						omrobjectptr_t next = NULL;
						MM_ForwardedHeader forwardHeader(object, compressed);
						Assert_MM_false(forwardHeader.isForwardedPointer());
						if (forwardHeader.isReverseForwardedPointer()) {
							omrobjectptr_t originalObject = forwardHeader.getReverseForwardedPointer();
							Assert_MM_true(NULL != originalObject);
							next = _extensions->accessBarrier->getFinalizeLink(originalObject);
							env->getGCEnvironment()->_unfinalizedObjectBuffer->add(env, originalObject);
						} else {
							next = _extensions->accessBarrier->getFinalizeLink(object);
							env->getGCEnvironment()->_unfinalizedObjectBuffer->add(env, object);
						}

						object = next;
					}
				}
			}
		}
	}

	/* restore everything to a flushed state before exiting */
	env->getGCEnvironment()->_unfinalizedObjectBuffer->flush(env);
}
#endif /* J9VM_GC_FINALIZATION */

void
MM_ScavengerBackOutScanner::backoutContinuationObjects(MM_EnvironmentStandard *env)
{
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	if (_extensions->isConcurrentScavengerEnabled()) {
		return;
	} else
#endif /* OMR_GC_CONCURRENT_SCAVENGER */
	{
		MM_Heap *heap = _extensions->heap;
		MM_HeapRegionManager *regionManager = heap->getHeapRegionManager();
		MM_HeapRegionDescriptorStandard *region = NULL;
		bool const compressed = _extensions->compressObjectReferences();
		GC_HeapRegionIteratorStandard regionIterator(regionManager);
		
		PORT_ACCESS_FROM_ENVIRONMENT(env);
//		j9tty_printf(PORTLIB, "backoutContinuationObjects env=%p\n", env);

		while (NULL != (region = regionIterator.nextRegion())) {

			j9tty_printf(PORTLIB, "backoutContinuationObjects region=%p, MEMORY_TYPE_NEW=%zu\n", region, (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)));

			if ((MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)) && _scavenger->isObjectInEvacuateMemory((omrobjectptr_t )region->getLowAddress())) {
				MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
				for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
					MM_ContinuationObjectList *list = &regionExtension->_continuationObjectLists[i];

					j9tty_printf(PORTLIB, "backoutContinuationObjects region=%p, continuationObjectLists[%zu]=%p, getPriorList=%p, getHeadOfList=%p\n", region, i, list, list->getPriorList(), list->getHeadOfList());

					if (!list->wasEmpty()) {
						omrobjectptr_t object = list->getPriorList();
						while (NULL != object) {
							omrobjectptr_t next = _extensions->accessBarrier->getContinuationLink(object);
							MM_ForwardedHeader forwardHeader(object, compressed);
							Assert_MM_false(forwardHeader.isForwardedPointer());
							Assert_MM_false(forwardHeader.isReverseForwardedPointer());
							_scavenger->getDelegate()->scanContinuationNativeSlots(env, object, SCAN_REASON_BACKOUT);

							object = next;
						}
					}
				}

			}
		}
	}
	/* Done backout */
}
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */
