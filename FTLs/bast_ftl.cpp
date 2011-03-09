/* Copyright 2011 Matias Bjørling */

/* bast_ftl.cpp  */

/* FlashSim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version. */

/* FlashSim is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License
 * along with FlashSim.  If not, see <http://www.gnu.org/licenses/>. */

/****************************************************************************/

/* Implementation of the BAST FTL described in the Paper
 * "A SPACE-EFFICIENT FLASH TRANSLATION LAYER FOR COMPACTFLASH SYSTEMS by Kim et. al."
 *
 * Notice: Startup procedures are not implemented as the drive is empty every time
 * the simulator is executed. i.e. OOB's is not filled with logical page address
 * at write and it is not read on startup to recreate mapping tables.
 */

#include <new>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../ssd.h"

using namespace ssd;

LogPageBlock::LogPageBlock()
{
	pages = new int[BLOCK_SIZE];

	for (uint i=0;i<BLOCK_SIZE;i++)
		pages[i] = -1;
}

void LogPageBlock::Reset()
{
	for (uint i=0;i<BLOCK_SIZE;i++)
		pages[i] = -1;

	address.valid = NONE;
}

LogPageBlock::~LogPageBlock()
{
	delete [] pages;
}

Ftl::Ftl(Controller &controller):
	controller(controller),
	garbage(*this),
	wear(*this)
{
	currentPage = 0;
	addressShift = 0;
	addressSize = 0;

	// Detect required number of bits for logical address size
	for (int size = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * 4; size > 0; addressSize++) size /= 2;

	// Find required number of bits for block size
	for (int size = BLOCK_SIZE/2;size > 0; addressShift++) size /= 2;

	printf("Total required bits for representation: %i (Address: %i Block: %i) \n", addressSize + addressShift, addressSize, addressShift);

	// Trivial assumption checks
	if (sizeof(int) != 4) assert("integer is not 4 bytes");

	// Initialise block mapping table.
	uint numBlocks = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE;
	data_list = new long[numBlocks];
	free_list = new long[numBlocks];
	log_list = new LogPageBlock[numBlocks];
	invalid_list = new long[numBlocks];

	for (uint i=0;i<numBlocks;i++)
	{
		data_list[i] = -1;
		free_list[i] = -1;
		invalid_list[i] = -1;
	}


	printf("Total mapping table size: %iKB\n", numBlocks * sizeof(uint) / 1024);
	printf("Using BAST FTL.\n");
	return;
}

Ftl::~Ftl(void)
{
	delete map;
	return;
}

enum status Ftl::read(Event &event)
{
	// Find block
	uint lookupBlock = (event.get_logical_address() >> addressShift);
	Address eventAddress;
	eventAddress.set_linear_address(event.get_logical_address());

	LogPageBlock *logBlock = &log_list[lookupBlock];

	if (data_list[lookupBlock] == -1 && logBlock->pages[eventAddress.page] == -1)
	{
		event.set_address(new Address(0, PAGE));
		fprintf(stderr, "Page read not written. Logical Address: %li\n", event.get_logical_address());
		return FAILURE;
	}

	// If page is in the log block
	if (logBlock->pages[eventAddress.page] != -1)
	{
		Address returnAddress = new Address(logBlock->address.get_linear_address()+logBlock->pages[eventAddress.page], PAGE);
		event.set_address(returnAddress);
		return controller.issue(event);
	} else {
		// If page is in the data block
		Address returnAddress = new Address(data_list[lookupBlock], PAGE);
		event.set_address(returnAddress);
		return controller.issue(event);
	}

	return FAILURE;
}

enum status Ftl::set_new_logblock(LogPageBlock *logBlock)
{
	// Get a new block and promote it as the block for the request.
	Address newLogBlock;
	if (get_free_block(newLogBlock) == FAILURE)
		return FAILURE;

	logBlock->address = new Address(newLogBlock.get_linear_address(), BLOCK);

	printf("Using new log block with address: %i\n", logBlock->address.get_linear_address());

	return SUCCESS;
}

enum status Ftl::write(Event &event)
{
	uint lookupBlock = (event.get_logical_address() >> addressShift);

	Address eventAddress;
	eventAddress.set_linear_address(event.get_logical_address());

	LogPageBlock *logBlock = &log_list[lookupBlock];

	// Go here when we initilize the log block.
	if (logBlock->address.valid == NONE)
		set_new_logblock(logBlock);

	block_state logBlockState = controller.get_block_state(logBlock->address);

	assert(logBlockState != INACTIVE);

	// Can it fit inside the existing log block. Issue the request.
	uint numValid = controller.get_num_valid(&logBlock->address);
	if (numValid < BLOCK_SIZE)
	{
		logBlock->pages[eventAddress.page] = numValid;

		Address logBlockAddress = logBlock->address;
		controller.get_free_page(logBlockAddress);

		event.set_address(logBlockAddress);

		return controller.issue(event);
	}


	// No page space. Merging required.
	/* 1. Log block merge
	 * 2. Log block switch
	 */

	// Is block switch possible? i.e. log block switch
	bool isSequential = true;
	for (uint i=0;i<BLOCK_SIZE;i++)
	{
		if (logBlock->pages[i] != i)
		{
			isSequential = false;
			break;
		}
	}

	if (isSequential)
	{
		// Add to empty list i.e. switch without erasing the datablock.
		if (data_list[lookupBlock] != -1)
			invalid_list[data_list[lookupBlock]] = 1; // Cleaned at next run.

		data_list[lookupBlock] = logBlock->address.get_linear_address();

		// Clear the log block for incoming I/Os to the same block.
		logBlock->Reset();

		set_new_logblock(logBlock);

		logBlock->pages[eventAddress.page] = 0;

		Address logBlockAddress = logBlock->address;
		controller.get_free_page(logBlockAddress);

		event.set_address(logBlockAddress);

		printf("Wrote sequential\n");
		// TODO: Update mapping with IO.
		return controller.issue(event);
	}


	// Do merge (n reads, n writes and 2 erases (gc'ed))
	/* 1. Write page to new data block
	 * 1a Promote new log block.
	 * 2. Create BLOCK_SIZE reads
	 * 3. Create BLOCK_SIZE writes
	 * 4. Invalidate data block
	 * 5. promote new block as data block
	 * 6. put data and log block into the invalidate list.
	 */

	Address newDataBlock2;
	if (get_free_block(newDataBlock2) == FAILURE)
		return FAILURE;

	Address newDataBlock = new Address(newDataBlock2.get_linear_address(), BLOCK);

	printf("Using new data block with address: %i\n", newDataBlock.get_linear_address());

	Address newLogBlock2;
	if (get_free_block(newLogBlock2) == FAILURE)
		return FAILURE;

	Address newLogBlock = new Address(newLogBlock2.get_linear_address(), BLOCK);

	printf("Using new log block with address: %i\n", newLogBlock.get_linear_address());

	// Write the current io to a new block.

	Address dataPage = newLogBlock;
	dataPage.valid = PAGE;
	event.set_address(dataPage);

	Event *eventOps = &event;
	Event *newEvent = NULL;
	for (uint i=0;i<BLOCK_SIZE;i++)
	{
		// Lookup page table and see if page exist in log page
		Address readAddress;
		if (logBlock->pages[eventAddress.page] != -1)
		{
			readAddress.set_linear_address(logBlock->address.real_address + logBlock->pages[i], PAGE);
		}
		else if (data_list[lookupBlock] != -1)
		{
			readAddress.set_linear_address(data_list[lookupBlock] + i, PAGE);
		}
		else
		{
			printf("Empty page.\n");
			continue;
		}

		if((newEvent = new Event(READ, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__);	exit(MEM_ERR);
		}

		newEvent->set_address(readAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;

		if((newEvent = new Event(WRITE, event.get_logical_address(), 1, event.get_start_time())) == NULL)
		{
			fprintf(stderr, "Ssd error: %s: could not allocate Event\n", __func__); exit(MEM_ERR);
		}

		Address dataBlockAddress = new Address(newDataBlock.get_linear_address() + i, PAGE);

		newEvent->set_payload(global_buffer);
		newEvent->set_address(dataBlockAddress);

		eventOps->set_next(*newEvent);
		eventOps = newEvent;
	}

	if (controller.issue(event) == FAILURE)
		return FAILURE;

	// Invalidate log and data block
	invalid_list[lookupBlock] = 1;
	if (data_list[lookupBlock] != -1)
		invalid_list[data_list[lookupBlock]] = 1;

	// Update mapping
	data_list[lookupBlock] = newDataBlock.get_linear_address();

	logBlock->Reset();
	logBlock->address = newLogBlock;
	logBlock->pages[eventAddress.page] = 0;

	event.consolidate_metaevent(event);

	return SUCCESS;
}


enum status Ftl::erase(Event &event)
{
	return SUCCESS;
}

enum status Ftl::merge(Event &event)
{
	return SUCCESS;
}

void Ftl::garbage_collect(Event &event)
{
	(void) garbage.collect(event);
}

ssd::ulong Ftl::get_erases_remaining(const Address &address) const
{
	return controller.get_erases_remaining(address);
}

void Ftl::get_least_worn(Address &address) const
{
	controller.get_least_worn(address);
	return;
}

enum page_state Ftl::get_state(const Address &address) const
{
	return controller.get_state(address);
}

enum status Ftl::get_free_block(Address &address)
{
	address.set_linear_address(currentPage, PAGE);

	currentPage += BLOCK_SIZE;
	if (controller.get_block_state(address) == FREE)
		return SUCCESS;

	fprintf(stderr, "No free pages left for FTL.\n");
	return FAILURE;
}
