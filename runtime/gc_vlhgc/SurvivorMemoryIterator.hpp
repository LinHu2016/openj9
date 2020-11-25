/*******************************************************************************
 * Copyright (c) 2020, 2020 IBM Corp. and others
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
#if !defined(SURVIVORMEMORYITERATOR_HPP_)
#define SURVIVORMEMORYITERATOR_HPP_

#include "omrmodroncore.h"

#include "EnvironmentVLHGC.hpp"
#include "GCExtensions.hpp"
#include "CompressedCardTable.hpp"
#include "HeapRegionDescriptorVLHGC.hpp"


/* Iterate memory chunks in non eden survivor region, if survivor == true, iterate free list, otherwise iterate occupied memory chunk */
class GC_SurvivorMemoryIterator
{
private:
	MM_HeapRegionDescriptorVLHGC *_region;
	bool _survivor;
	UDATA *_compressedSurvivorTable;
	UDATA _heapBase;
	UDATA _index;
	UDATA _indexRegionBase;
	UDATA _indexRegionTop;
	void *_currentLow;
	void *_currentHigh;

protected:
public:
	/**
	 * Construct Iterator for a given region
	 *
	 * @param region
	 * @param survivor
	 */
	GC_SurvivorMemoryIterator(MM_EnvironmentVLHGC *env, MM_HeapRegionDescriptorVLHGC *region, bool survivor = true)
		: _region(region)
		, _survivor(survivor)
	{
		_compressedSurvivorTable = MM_GCExtensions::getExtensions(env)->compressedCardTable->getCompressedSurvivorTable();
		_heapBase = (UDATA) MM_GCExtensions::getExtensions(env)->heap->getHeapBase();
		_indexRegionBase = ((UDATA)_region->getLowAddress() -  _heapBase) / CARD_SIZE;
		_indexRegionTop  = ((UDATA)_region->getHighAddress() - _heapBase) / CARD_SIZE;
		_index = _indexRegionBase;
		_currentLow = NULL;
		_currentHigh = NULL;
	}

	bool next()
	{
		UDATA mask = 1;
		UDATA compressedIndex = _index / COMPRESSED_CARDS_PER_WORD;
		UDATA *compressedSurvivor = &_compressedSurvivorTable[compressedIndex];
		UDATA compressedSurvivorWord = compressedSurvivor[0];
		const UDATA endOfWord = ((UDATA)1) << (COMPRESSED_CARDS_PER_WORD - 1);
		_currentLow = NULL;
		_currentHigh = NULL;
		bool found = false;

		while((_index < _indexRegionTop) && !found) {
			if ((_survivor && (0 != (compressedSurvivorWord&mask))) ||
				(!_survivor && (0 == (compressedSurvivorWord&mask)))) {
				_currentLow = (void *)(_index * CARD_SIZE + _heapBase);
				found = true;
			}
			if (mask == endOfWord) {
				compressedSurvivor++;
				compressedSurvivorWord = compressedSurvivor[0];
				mask = 1;
			} else {
				/* mask for next bit to handle */
				mask = mask << 1;
			}
			_index++;
		}
		if (NULL == _currentLow) {
			while(_index < _indexRegionTop) {
				if ((!_survivor && (0 != (compressedSurvivorWord&mask))) ||
					(_survivor && (0 == (compressedSurvivorWord&mask)))) {
					break;
				}
				if (mask == endOfWord) {
					compressedSurvivor++;
					compressedSurvivorWord = compressedSurvivor[0];
					mask = 1;
				} else {
					/* mask for next bit to handle */
					mask = mask << 1;
				}
				_index++;
			}
			_currentHigh = (void *)(_index * CARD_SIZE + _heapBase);
		}
		return found;
	}

	void* getCurrentLow()
	{
		return _currentLow;
	}

	void* getCurrentHigh()
	{
		return _currentHigh;
	}
};

#endif /* SURVIVORMEMORYITERATOR_HPP_ */
