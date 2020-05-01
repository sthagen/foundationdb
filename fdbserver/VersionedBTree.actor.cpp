/*
 * VersionedBTree.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/flow.h"
#include "fdbserver/IVersionedStore.h"
#include "fdbserver/IPager.h"
#include "fdbclient/Tuple.h"
#include "flow/serialize.h"
#include "flow/genericactors.actor.h"
#include "flow/UnitTest.h"
#include "fdbserver/IPager.h"
#include "fdbrpc/IAsyncFile.h"
#include "flow/crc32c.h"
#include "flow/ActorCollection.h"
#include <map>
#include <vector>
#include "fdbclient/CommitTransaction.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/DeltaTree.h"
#include <string.h>
#include "flow/actorcompiler.h"
#include <cinttypes>
#include <boost/intrusive/list.hpp>

// Some convenience functions for debugging to stringify various structures
// Classes can add compatibility by either specializing toString<T> or implementing
//   std::string toString() const;
template <typename T>
std::string toString(const T& o) {
	return o.toString();
}

std::string toString(StringRef s) {
	return s.printable();
}

std::string toString(LogicalPageID id) {
	if (id == invalidLogicalPageID) {
		return "LogicalPageID{invalid}";
	}
	return format("LogicalPageID{%" PRId64 "}", id);
}

template <typename T>
std::string toString(const Standalone<T>& s) {
	return toString((T)s);
}

template <typename T>
std::string toString(const T* begin, const T* end) {
	std::string r = "{";

	bool comma = false;
	while (begin != end) {
		if (comma) {
			r += ", ";
		} else {
			comma = true;
		}
		r += toString(*begin++);
	}

	r += "}";
	return r;
}

template <typename T>
std::string toString(const std::vector<T>& v) {
	return toString(&v.front(), &v.back() + 1);
}

template <typename T>
std::string toString(const VectorRef<T>& v) {
	return toString(v.begin(), v.end());
}

template <typename T>
std::string toString(const Optional<T>& o) {
	if (o.present()) {
		return toString(o.get());
	}
	return "<not present>";
}

// A FIFO queue of T stored as a linked list of pages.
// Main operations are pop(), pushBack(), pushFront(), and flush().
//
// flush() will ensure all queue pages are written to the pager and move the unflushed
// pushFront()'d records onto the front of the queue, in FIFO order.
//
// pop() will only return records that have been flushed, and pops
// from the front of the queue.
//
// Each page contains some number of T items and a link to the next page and starting position on that page.
// When the queue is flushed, the last page in the chain is ended and linked to a newly allocated
// but not-yet-written-to pageID, which future writes after the flush will write to.
// Items pushed onto the front of the queue are written to a separate linked list until flushed,
// at which point that list becomes the new front of the queue.
//
// The write pattern is designed such that no page is ever expected to be valid after
// being written to or updated but not fsync'd.  This is why a new unused page is added
// to the queue, linked to by the last data page, before commit.  The new page can't be
// added and filled with data as part of the next commit because that would mean modifying
// the previous tail page to update its next link, which risks corrupting it and losing
// data that was not yet popped if that write is never fsync'd.
//
// Requirements on T
//   - must be trivially copyable
//     OR have a specialization for FIFOQueueCodec<T>
//     OR have the following methods
//       // Deserialize from src into *this, return number of bytes from src consumed
//       int readFromBytes(const uint8_t *src);
//       // Return the size of *this serialized
//       int bytesNeeded() const;
//       // Serialize *this to dst, return number of bytes written to dst
//       int writeToBytes(uint8_t *dst) const;
//  - must be supported by toString(object) (see above)
template <typename T, typename Enable = void>
struct FIFOQueueCodec {
	static T readFromBytes(const uint8_t* src, int& bytesRead) {
		T x;
		bytesRead = x.readFromBytes(src);
		return x;
	}
	static int bytesNeeded(const T& x) { return x.bytesNeeded(); }
	static int writeToBytes(uint8_t* dst, const T& x) { return x.writeToBytes(dst); }
};

template <typename T>
struct FIFOQueueCodec<T, typename std::enable_if<std::is_trivially_copyable<T>::value>::type> {
	static_assert(std::is_trivially_copyable<T>::value);
	static T readFromBytes(const uint8_t* src, int& bytesRead) {
		bytesRead = sizeof(T);
		return *(T*)src;
	}
	static int bytesNeeded(const T& x) { return sizeof(T); }
	static int writeToBytes(uint8_t* dst, const T& x) {
		*(T*)dst = x;
		return sizeof(T);
	}
};

template <typename T, typename Codec = FIFOQueueCodec<T>>
class FIFOQueue {
public:
#pragma pack(push, 1)
	struct QueueState {
		bool operator==(const QueueState& rhs) const { return memcmp(this, &rhs, sizeof(QueueState)) == 0; }
		LogicalPageID headPageID = invalidLogicalPageID;
		LogicalPageID tailPageID = invalidLogicalPageID;
		uint16_t headOffset;
		// Note that there is no tail index because the tail page is always never-before-written and its index will
		// start at 0
		int64_t numPages;
		int64_t numEntries;
		std::string toString() const {
			return format("{head: %s:%d  tail: %s  numPages: %" PRId64 "  numEntries: %" PRId64 "}",
			              ::toString(headPageID).c_str(), (int)headOffset, ::toString(tailPageID).c_str(), numPages,
			              numEntries);
		}
	};
#pragma pack(pop)

	struct Cursor {
		enum Mode { NONE, POP, READONLY, WRITE };

		// The current page being read or written to
		LogicalPageID pageID;

		// The first page ID to be written to the pager, if this cursor has written anything
		LogicalPageID firstPageIDWritten;

		// Offset after RawPage header to next read from or write to
		int offset;

		// A read cursor will not read this page (or beyond)
		LogicalPageID endPageID;

		Reference<IPage> page;
		FIFOQueue* queue;
		Future<Void> operation;
		Mode mode;

		Cursor() : mode(NONE) {}

		// Initialize a cursor.
		void init(FIFOQueue* q = nullptr, Mode m = NONE, LogicalPageID initialPageID = invalidLogicalPageID,
		          int readOffset = 0, LogicalPageID endPage = invalidLogicalPageID) {
			if (operation.isValid()) {
				operation.cancel();
			}
			queue = q;
			mode = m;
			firstPageIDWritten = invalidLogicalPageID;
			offset = readOffset;
			endPageID = endPage;
			page.clear();

			if (mode == POP || mode == READONLY) {
				// If cursor is not pointed at the end page then start loading it.
				// The end page will not have been written to disk yet.
				pageID = initialPageID;
				operation = (pageID == endPageID) ? Void() : loadPage();
			} else {
				pageID = invalidLogicalPageID;
				ASSERT(mode == WRITE ||
				       (initialPageID == invalidLogicalPageID && readOffset == 0 && endPage == invalidLogicalPageID));
				operation = Void();
			}

			debug_printf("FIFOQueue::Cursor(%s) initialized\n", toString().c_str());

			if (mode == WRITE && initialPageID != invalidLogicalPageID) {
				addNewPage(initialPageID, 0, true);
			}
		}

		// Since cursors can have async operations pending which modify their state they can't be copied cleanly
		Cursor(const Cursor& other) = delete;

		// A read cursor can be initialized from a pop cursor
		void initReadOnly(const Cursor& c) {
			ASSERT(c.mode == READONLY || c.mode == POP);
			init(c.queue, READONLY, c.pageID, c.offset, c.endPageID);
		}

		~Cursor() { operation.cancel(); }

		std::string toString() const {
			if (mode == WRITE) {
				return format("{WriteCursor %s:%p pos=%s:%d endOffset=%d}", queue->name.c_str(), this,
				              ::toString(pageID).c_str(), offset, page ? raw()->endOffset : -1);
			}
			if (mode == POP || mode == READONLY) {
				return format("{ReadCursor %s:%p pos=%s:%d endOffset=%d endPage=%s}", queue->name.c_str(), this,
				              ::toString(pageID).c_str(), offset, page ? raw()->endOffset : -1,
				              ::toString(endPageID).c_str());
			}
			ASSERT(mode == NONE);
			return format("{NullCursor=%p}", this);
		}

#pragma pack(push, 1)
		struct RawPage {
			LogicalPageID nextPageID;
			uint16_t nextOffset;
			uint16_t endOffset;
			uint8_t* begin() { return (uint8_t*)(this + 1); }
		};
#pragma pack(pop)

		Future<Void> notBusy() { return operation; }

		// Returns true if any items have been written to the last page
		bool pendingWrites() const { return mode == WRITE && offset != 0; }

		RawPage* raw() const { return ((RawPage*)(page->begin())); }

		void setNext(LogicalPageID pageID, int offset) {
			ASSERT(mode == WRITE);
			RawPage* p = raw();
			p->nextPageID = pageID;
			p->nextOffset = offset;
		}

		Future<Void> loadPage() {
			ASSERT(mode == POP | mode == READONLY);
			debug_printf("FIFOQueue::Cursor(%s) loadPage\n", toString().c_str());
			return map(queue->pager->readPage(pageID, true), [=](Reference<IPage> p) {
				page = p;
				debug_printf("FIFOQueue::Cursor(%s) loadPage done\n", toString().c_str());
				return Void();
			});
		}

		void writePage() {
			ASSERT(mode == WRITE);
			debug_printf("FIFOQueue::Cursor(%s) writePage\n", toString().c_str());
			VALGRIND_MAKE_MEM_DEFINED(raw()->begin(), offset);
			VALGRIND_MAKE_MEM_DEFINED(raw()->begin() + offset, queue->dataBytesPerPage - raw()->endOffset);
			queue->pager->updatePage(pageID, page);
			if (firstPageIDWritten == invalidLogicalPageID) {
				firstPageIDWritten = pageID;
			}
		}

		// Link the current page to newPageID:newOffset and then write it to the pager.
		// If initializeNewPage is true a page buffer will be allocated for the new page and it will be initialized
		// as a new tail page.
		void addNewPage(LogicalPageID newPageID, int newOffset, bool initializeNewPage) {
			ASSERT(mode == WRITE);
			ASSERT(newPageID != invalidLogicalPageID);
			debug_printf("FIFOQueue::Cursor(%s) Adding page %s init=%d\n", toString().c_str(),
			             ::toString(newPageID).c_str(), initializeNewPage);

			// Update existing page and write, if it exists
			if (page) {
				setNext(newPageID, newOffset);
				debug_printf("FIFOQueue::Cursor(%s) Linked new page\n", toString().c_str());
				writePage();
			}

			pageID = newPageID;
			offset = newOffset;

			if (initializeNewPage) {
				debug_printf("FIFOQueue::Cursor(%s) Initializing new page\n", toString().c_str());
				page = queue->pager->newPageBuffer();
				setNext(0, 0);
				auto p = raw();
				ASSERT(newOffset == 0);
				p->endOffset = 0;
			} else {
				page.clear();
			}
		}

		// Write item to the next position in the current page or, if it won't fit, add a new page and write it there.
		ACTOR static Future<Void> write_impl(Cursor* self, T item, Future<Void> start) {
			ASSERT(self->mode == WRITE);

			// Wait for the previous operation to finish
			state Future<Void> previous = self->operation;
			wait(start);
			wait(previous);

			state int bytesNeeded = Codec::bytesNeeded(item);
			if (self->pageID == invalidLogicalPageID || self->offset + bytesNeeded > self->queue->dataBytesPerPage) {
				debug_printf("FIFOQueue::Cursor(%s) write(%s) page is full, adding new page\n",
				             self->toString().c_str(), ::toString(item).c_str());
				LogicalPageID newPageID = wait(self->queue->pager->newPageID());
				self->addNewPage(newPageID, 0, true);
				++self->queue->numPages;
				wait(yield());
			}
			debug_printf("FIFOQueue::Cursor(%s) before write(%s)\n", self->toString().c_str(),
			             ::toString(item).c_str());
			auto p = self->raw();
			Codec::writeToBytes(p->begin() + self->offset, item);
			self->offset += bytesNeeded;
			p->endOffset = self->offset;
			++self->queue->numEntries;
			return Void();
		}

		void write(const T& item) {
			Promise<Void> p;
			operation = write_impl(this, item, p.getFuture());
			p.send(Void());
		}

		// Read the next item at the cursor (if <= upperBound), moving to a new page first if the current page is
		// exhausted
		ACTOR static Future<Optional<T>> readNext_impl(Cursor* self, Optional<T> upperBound, Future<Void> start) {
			ASSERT(self->mode == POP || self->mode == READONLY);

			// Wait for the previous operation to finish
			state Future<Void> previous = self->operation;
			wait(start);
			wait(previous);

			debug_printf("FIFOQueue::Cursor(%s) readNext begin\n", self->toString().c_str());
			if (self->pageID == invalidLogicalPageID || self->pageID == self->endPageID) {
				debug_printf("FIFOQueue::Cursor(%s) readNext returning nothing\n", self->toString().c_str());
				return Optional<T>();
			}

			// We now know we are pointing to PageID and it should be read and used, but it may not be loaded yet.
			if (!self->page) {
				wait(self->loadPage());
				wait(yield());
			}

			auto p = self->raw();
			debug_printf("FIFOQueue::Cursor(%s) readNext reading at current position\n", self->toString().c_str());
			ASSERT(self->offset < p->endOffset);
			int bytesRead;
			T result = Codec::readFromBytes(p->begin() + self->offset, bytesRead);

			if (upperBound.present() && upperBound.get() < result) {
				debug_printf("FIFOQueue::Cursor(%s) not popping %s, exceeds upper bound %s\n", self->toString().c_str(),
				             ::toString(result).c_str(), ::toString(upperBound.get()).c_str());
				return Optional<T>();
			}

			self->offset += bytesRead;
			if (self->mode == POP) {
				--self->queue->numEntries;
			}
			debug_printf("FIFOQueue::Cursor(%s) after read of %s\n", self->toString().c_str(),
			             ::toString(result).c_str());
			ASSERT(self->offset <= p->endOffset);

			if (self->offset == p->endOffset) {
				debug_printf("FIFOQueue::Cursor(%s) Page exhausted\n", self->toString().c_str());
				LogicalPageID oldPageID = self->pageID;
				self->pageID = p->nextPageID;
				self->offset = p->nextOffset;
				if (self->mode == POP) {
					--self->queue->numPages;
				}
				self->page.clear();
				debug_printf("FIFOQueue::Cursor(%s) readNext page exhausted, moved to new page\n",
				             self->toString().c_str());

				if (self->mode == POP) {
					// Freeing the old page must happen after advancing the cursor and clearing the page reference
					// because freePage() could cause a push onto a queue that causes a newPageID() call which could
					// pop() from this very same queue. Queue pages are freed at page 0 because they can be reused after
					// the next commit.
					self->queue->pager->freePage(oldPageID, 0);
				}
			}

			debug_printf("FIFOQueue(%s) %s(upperBound=%s) -> %s\n", self->queue->name.c_str(),
			             (self->mode == POP ? "pop" : "peek"), ::toString(upperBound).c_str(),
			             ::toString(result).c_str());
			return result;
		}

		// Read and move past the next item if is <= upperBound or if upperBound is not present
		Future<Optional<T>> readNext(const Optional<T>& upperBound = {}) {
			if (mode == NONE) {
				return Optional<T>();
			}
			Promise<Void> p;
			Future<Optional<T>> read = readNext_impl(this, upperBound, p.getFuture());
			operation = success(read);
			p.send(Void());
			return read;
		}
	};

public:
	FIFOQueue() : pager(nullptr) {}

	~FIFOQueue() { newTailPage.cancel(); }

	FIFOQueue(const FIFOQueue& other) = delete;
	void operator=(const FIFOQueue& rhs) = delete;

	// Create a new queue at newPageID
	void create(IPager2* p, LogicalPageID newPageID, std::string queueName) {
		debug_printf("FIFOQueue(%s) create from page %s\n", queueName.c_str(), toString(newPageID).c_str());
		pager = p;
		name = queueName;
		numPages = 1;
		numEntries = 0;
		dataBytesPerPage = pager->getUsablePageSize() - sizeof(typename Cursor::RawPage);
		headReader.init(this, Cursor::POP, newPageID, 0, newPageID);
		tailWriter.init(this, Cursor::WRITE, newPageID);
		headWriter.init(this, Cursor::WRITE);
		newTailPage = invalidLogicalPageID;
		debug_printf("FIFOQueue(%s) created\n", queueName.c_str());
	}

	// Load an existing queue from its queue state
	void recover(IPager2* p, const QueueState& qs, std::string queueName) {
		debug_printf("FIFOQueue(%s) recover from queue state %s\n", queueName.c_str(), qs.toString().c_str());
		pager = p;
		name = queueName;
		numPages = qs.numPages;
		numEntries = qs.numEntries;
		dataBytesPerPage = pager->getUsablePageSize() - sizeof(typename Cursor::RawPage);
		headReader.init(this, Cursor::POP, qs.headPageID, qs.headOffset, qs.tailPageID);
		tailWriter.init(this, Cursor::WRITE, qs.tailPageID);
		headWriter.init(this, Cursor::WRITE);
		newTailPage = invalidLogicalPageID;
		debug_printf("FIFOQueue(%s) recovered\n", queueName.c_str());
	}

	ACTOR static Future<Standalone<VectorRef<T>>> peekAll_impl(FIFOQueue* self) {
		state Standalone<VectorRef<T>> results;
		state Cursor c;
		c.initReadOnly(self->headReader);
		results.reserve(results.arena(), self->numEntries);

		loop {
			Optional<T> x = wait(c.readNext());
			if (!x.present()) {
				break;
			}
			results.push_back(results.arena(), x.get());
		}

		return results;
	}

	Future<Standalone<VectorRef<T>>> peekAll() { return peekAll_impl(this); }

	// Pop the next item on front of queue if it is <= upperBound or if upperBound is not present
	Future<Optional<T>> pop(Optional<T> upperBound = {}) { return headReader.readNext(upperBound); }

	QueueState getState() const {
		QueueState s;
		s.headOffset = headReader.offset;
		s.headPageID = headReader.pageID;
		s.tailPageID = tailWriter.pageID;
		s.numEntries = numEntries;
		s.numPages = numPages;

		debug_printf("FIFOQueue(%s) getState(): %s\n", name.c_str(), s.toString().c_str());
		return s;
	}

	void pushBack(const T& item) {
		debug_printf("FIFOQueue(%s) pushBack(%s)\n", name.c_str(), toString(item).c_str());
		tailWriter.write(item);
	}

	void pushFront(const T& item) {
		debug_printf("FIFOQueue(%s) pushFront(%s)\n", name.c_str(), toString(item).c_str());
		headWriter.write(item);
	}

	// Wait until the most recently started operations on each cursor as of now are ready
	Future<Void> notBusy() {
		return headWriter.notBusy() && headReader.notBusy() && tailWriter.notBusy() && ready(newTailPage);
	}

	// Returns true if any most recently started operations on any cursors are not ready
	bool busy() {
		return !headWriter.notBusy().isReady() || !headReader.notBusy().isReady() || !tailWriter.notBusy().isReady() ||
		       !newTailPage.isReady();
	}

	// preFlush() prepares this queue to be flushed to disk, but doesn't actually do it so the queue can still
	// be pushed and popped after this operation. It returns whether or not any operations were pending or
	// started during execution.
	//
	// If one or more queues are used by their pager in newPageID() or freePage() operations, then preFlush()
	// must be called on each of them inside a loop that runs until each of the preFlush() calls have returned
	// false.
	//
	// The reason for all this is that:
	//   - queue pop() can call pager->freePage() which can call push() on the same or another queue
	//   - queue push() can call pager->newPageID() which can call pop() on the same or another queue
	// This creates a circular dependency with 1 or more queues when those queues are used by the pager
	// to manage free page IDs.
	ACTOR static Future<bool> preFlush_impl(FIFOQueue* self) {
		debug_printf("FIFOQueue(%s) preFlush begin\n", self->name.c_str());
		wait(self->notBusy());

		// Completion of the pending operations as of the start of notBusy() could have began new operations,
		// so see if any work is pending now.
		bool workPending = self->busy();

		if (!workPending) {
			// A newly created or flushed queue starts out in a state where its tail page to be written to is empty.
			// After pushBack() is called, this is no longer the case and never will be again until the queue is
			// flushed. Before the non-empty tail page is written it must be linked to a new empty page for use after
			// the next flush.  (This is explained more at the top of FIFOQueue but it is because queue pages can only
			// be written once because once they contain durable data a second write to link to a new page could corrupt
			// the existing data if the subsequent commit never succeeds.)
			if (self->newTailPage.isReady() && self->newTailPage.get() == invalidLogicalPageID &&
			    self->tailWriter.pendingWrites()) {
				self->newTailPage = self->pager->newPageID();
				workPending = true;
			}
		}

		debug_printf("FIFOQueue(%s) preFlush returning %d\n", self->name.c_str(), workPending);
		return workPending;
	}

	Future<bool> preFlush() { return preFlush_impl(this); }

	void finishFlush() {
		debug_printf("FIFOQueue(%s) finishFlush start\n", name.c_str());
		ASSERT(!busy());

		// If a new tail page was allocated, link the last page of the tail writer to it.
		if (newTailPage.get() != invalidLogicalPageID) {
			tailWriter.addNewPage(newTailPage.get(), 0, false);
			// The flush sequence allocated a page and added it to the queue so increment numPages
			++numPages;

			// newPage() should be ready immediately since a pageID is being explicitly passed.
			ASSERT(tailWriter.notBusy().isReady());

			newTailPage = invalidLogicalPageID;
		}

		// If the headWriter wrote anything, link its tail page to the headReader position and point the headReader
		// to the start of the headWriter
		if (headWriter.pendingWrites()) {
			headWriter.addNewPage(headReader.pageID, headReader.offset, false);
			headReader.pageID = headWriter.firstPageIDWritten;
			headReader.offset = 0;
			headReader.page.clear();
		}

		// Update headReader's end page to the new tail page
		headReader.endPageID = tailWriter.pageID;

		// Reset the write cursors
		tailWriter.init(this, Cursor::WRITE, tailWriter.pageID);
		headWriter.init(this, Cursor::WRITE);

		debug_printf("FIFOQueue(%s) finishFlush end\n", name.c_str());
	}

	ACTOR static Future<Void> flush_impl(FIFOQueue* self) {
		loop {
			bool notDone = wait(self->preFlush());
			if (!notDone) {
				break;
			}
		}
		self->finishFlush();
		return Void();
	}

	Future<Void> flush() { return flush_impl(this); }

	IPager2* pager;
	int64_t numPages;
	int64_t numEntries;
	int dataBytesPerPage;

	Cursor headReader;
	Cursor tailWriter;
	Cursor headWriter;

	Future<LogicalPageID> newTailPage;

	// For debugging
	std::string name;
};

int nextPowerOf2(uint32_t x) {
	return 1 << (32 - clz(x - 1));
}

class FastAllocatedPage : public IPage, public FastAllocated<FastAllocatedPage>, ReferenceCounted<FastAllocatedPage> {
public:
	// Create a fast-allocated page with size total bytes INCLUDING checksum
	FastAllocatedPage(int size, int bufferSize) : logicalSize(size), bufferSize(bufferSize) {
		buffer = (uint8_t*)allocateFast(bufferSize);
		// Mark any unused page portion defined
		VALGRIND_MAKE_MEM_DEFINED(buffer + logicalSize, bufferSize - logicalSize);
	};

	virtual ~FastAllocatedPage() { freeFast(bufferSize, buffer); }

	virtual Reference<IPage> clone() const {
		FastAllocatedPage* p = new FastAllocatedPage(logicalSize, bufferSize);
		memcpy(p->buffer, buffer, logicalSize);
		return Reference<IPage>(p);
	}

	// Usable size, without checksum
	int size() const { return logicalSize - sizeof(Checksum); }

	uint8_t const* begin() const { return buffer; }

	uint8_t* mutate() { return buffer; }

	void addref() const { ReferenceCounted<FastAllocatedPage>::addref(); }

	void delref() const { ReferenceCounted<FastAllocatedPage>::delref(); }

	typedef uint32_t Checksum;

	Checksum& getChecksum() { return *(Checksum*)(buffer + size()); }

	Checksum calculateChecksum(LogicalPageID pageID) { return crc32c_append(pageID, buffer, size()); }

	void updateChecksum(LogicalPageID pageID) { getChecksum() = calculateChecksum(pageID); }

	bool verifyChecksum(LogicalPageID pageID) { return getChecksum() == calculateChecksum(pageID); }

private:
	int logicalSize;
	int bufferSize;
	uint8_t* buffer;
};

// Holds an index of recently used objects.
// ObjectType must have the method
//   bool evictable() const;            // return true if the entry can be evicted
//   Future<Void> onEvictable() const;  // ready when entry can be evicted
// indicating if it is safe to evict.
template <class IndexType, class ObjectType>
class ObjectCache : NonCopyable {

	struct Entry : public boost::intrusive::list_base_hook<> {
		Entry() : hits(0) {}
		IndexType index;
		ObjectType item;
		int hits;
	};

	typedef std::unordered_map<IndexType, Entry> CacheT;
	typedef boost::intrusive::list<Entry> EvictionOrderT;

public:
	ObjectCache(int sizeLimit = 1)
	  : sizeLimit(sizeLimit), cacheHits(0), cacheMisses(0), noHitEvictions(0), failedEvictions(0) {}

	void setSizeLimit(int n) {
		ASSERT(n > 0);
		sizeLimit = n;
	}

	// Get the object for i if it exists, else return nullptr.
	// If the object exists, its eviction order will NOT change as this is not a cache hit.
	ObjectType* getIfExists(const IndexType& index) {
		auto i = cache.find(index);
		if (i != cache.end()) {
			++i->second.hits;
			return &i->second.item;
		}
		return nullptr;
	}

	// Get the object for i or create a new one.
	// After a get(), the object for i is the last in evictionOrder.
	ObjectType& get(const IndexType& index, bool noHit = false) {
		Entry& entry = cache[index];

		// If entry is linked into evictionOrder then move it to the back of the order
		if (entry.is_linked()) {
			if (!noHit) {
				++entry.hits;
				++cacheHits;
			}
			// Move the entry to the back of the eviction order
			evictionOrder.erase(evictionOrder.iterator_to(entry));
			evictionOrder.push_back(entry);
		} else {
			++cacheMisses;
			// Finish initializing entry
			entry.index = index;
			entry.hits = noHit ? 0 : 1;
			// Insert the newly created Entry at the back of the eviction order
			evictionOrder.push_back(entry);

			// While the cache is too big, evict the oldest entry until the oldest entry can't be evicted.
			while (cache.size() > sizeLimit) {
				Entry& toEvict = evictionOrder.front();
				debug_printf("Trying to evict %s to make room for %s\n", toString(toEvict.index).c_str(),
				             toString(index).c_str());

				// It's critical that we do not evict the item we just added (or the reference we return would be
				// invalid) but since sizeLimit must be > 0, entry was just added to the end of the evictionOrder, and
				// this loop will end if we move anything to the end of the eviction order, we can be guaraunted that
				// entry != toEvict, so we do not need to check. If the item is not evictable then move it to the back
				// of the eviction order and stop.
				if (!toEvict.item.evictable()) {
					evictionOrder.erase(evictionOrder.iterator_to(toEvict));
					evictionOrder.push_back(toEvict);
					++failedEvictions;
					break;
				} else {
					if (toEvict.hits == 0) {
						++noHitEvictions;
					}
					debug_printf("Evicting %s to make room for %s\n", toString(toEvict.index).c_str(),
					             toString(index).c_str());
					evictionOrder.pop_front();
					cache.erase(toEvict.index);
				}
			}
		}

		return entry.item;
	}

	// Clears the cache, saving the entries, and then waits for eachWaits for each item to be evictable and evicts it.
	// The cache should not be Evicts all evictable entries
	ACTOR static Future<Void> clear_impl(ObjectCache* self) {
		state ObjectCache::CacheT cache;
		state EvictionOrderT evictionOrder;

		// Swap cache contents to local state vars
		// After this, no more entries will be added to or read from these
		// structures so we know for sure that no page will become unevictable
		// after it is either evictable or onEvictable() is ready.
		cache.swap(self->cache);
		evictionOrder.swap(self->evictionOrder);

		state typename EvictionOrderT::iterator i = evictionOrder.begin();
		state typename EvictionOrderT::iterator iEnd = evictionOrder.begin();

		while (i != iEnd) {
			if (!i->item.evictable()) {
				wait(i->item.onEvictable());
			}
			++i;
		}

		evictionOrder.clear();
		cache.clear();

		return Void();
	}

	Future<Void> clear() { return clear_impl(this); }

	int count() const {
		ASSERT(evictionOrder.size() == cache.size());
		return evictionOrder.size();
	}

private:
	int64_t sizeLimit;
	int64_t cacheHits;
	int64_t cacheMisses;
	int64_t noHitEvictions;
	int64_t failedEvictions;

	CacheT cache;
	EvictionOrderT evictionOrder;
};

ACTOR template <class T>
Future<T> forwardError(Future<T> f, Promise<Void> target) {
	try {
		T x = wait(f);
		return x;
	} catch (Error& e) {
		if (e.code() != error_code_actor_cancelled && target.canBeSet()) {
			target.sendError(e);
		}

		throw e;
	}
}

class DWALPagerSnapshot;

// An implementation of IPager2 that supports atomicUpdate() of a page without forcing a change to new page ID.
// It does this internally mapping the original page ID to alternate page IDs by write version.
// The page id remaps are kept in memory and also logged to a "remap queue" which must be reloaded on cold start.
// To prevent the set of remaps from growing unboundedly, once a remap is old enough to be at or before the
// oldest pager version being maintained the remap can be "undone" by popping it from the remap queue,
// copying the alternate page ID's data over top of the original page ID's data, and deleting the remap from memory.
// This process basically describes a "Delayed" Write-Ahead-Log (DWAL) because the remap queue and the newly allocated
// alternate pages it references basically serve as a write ahead log for pages that will eventially be copied
// back to their original location once the original version is no longer needed.
class DWALPager : public IPager2 {
public:
	typedef FastAllocatedPage Page;
	typedef FIFOQueue<LogicalPageID> LogicalPageQueueT;

#pragma pack(push, 1)
	struct DelayedFreePage {
		Version version;
		LogicalPageID pageID;

		bool operator<(const DelayedFreePage& rhs) const { return version < rhs.version; }

		std::string toString() const {
			return format("DelayedFreePage{%s @%" PRId64 "}", ::toString(pageID).c_str(), version);
		}
	};

	struct RemappedPage {
		Version version;
		LogicalPageID originalPageID;
		LogicalPageID newPageID;

		bool operator<(const RemappedPage& rhs) { return version < rhs.version; }

		std::string toString() const {
			return format("RemappedPage(%s -> %s @%" PRId64 "}", ::toString(originalPageID).c_str(),
			              ::toString(newPageID).c_str(), version);
		}
	};

#pragma pack(pop)

	typedef FIFOQueue<DelayedFreePage> DelayedFreePageQueueT;
	typedef FIFOQueue<RemappedPage> RemapQueueT;

	// If the file already exists, pageSize might be different than desiredPageSize
	// Use pageCacheSizeBytes == 0 for default
	DWALPager(int desiredPageSize, std::string filename, int64_t pageCacheSizeBytes)
	  : desiredPageSize(desiredPageSize), filename(filename), pHeader(nullptr), pageCacheBytes(pageCacheSizeBytes) {
		if (pageCacheBytes == 0) {
			pageCacheBytes = g_network->isSimulated()
			                     ? (BUGGIFY ? FLOW_KNOBS->BUGGIFY_SIM_PAGE_CACHE_4K : FLOW_KNOBS->SIM_PAGE_CACHE_4K)
			                     : FLOW_KNOBS->PAGE_CACHE_4K;
		}
		commitFuture = Void();
		recoverFuture = forwardError(recover(this), errorPromise);
	}

	void setPageSize(int size) {
		logicalPageSize = size;
		physicalPageSize = smallestPhysicalBlock;
		while (logicalPageSize > physicalPageSize) {
			physicalPageSize += smallestPhysicalBlock;
		}
		if (pHeader != nullptr) {
			pHeader->pageSize = logicalPageSize;
		}
		pageCache.setSizeLimit(pageCacheBytes / physicalPageSize);
	}

	void updateCommittedHeader() {
		memcpy(lastCommittedHeaderPage->mutate(), headerPage->begin(), smallestPhysicalBlock);
	}

	ACTOR static Future<Void> recover(DWALPager* self) {
		ASSERT(!self->recoverFuture.isValid());

		self->remapUndoFuture = Void();

		int64_t flags = IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_UNBUFFERED | IAsyncFile::OPEN_READWRITE |
		                IAsyncFile::OPEN_LOCK;
		state bool exists = fileExists(self->filename);
		if (!exists) {
			flags |= IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_CREATE;
		}

		wait(store(self->pageFile, IAsyncFileSystem::filesystem()->open(self->filename, flags, 0644)));

		// Header page is always treated as having a page size of smallestPhysicalBlock
		self->setPageSize(smallestPhysicalBlock);
		self->lastCommittedHeaderPage = self->newPageBuffer();
		self->pLastCommittedHeader = (Header*)self->lastCommittedHeaderPage->begin();

		state int64_t fileSize = 0;
		if (exists) {
			wait(store(fileSize, self->pageFile->size()));
		}

		debug_printf("DWALPager(%s) recover exists=%d fileSize=%" PRId64 "\n", self->filename.c_str(), exists,
		             fileSize);
		// TODO:  If the file exists but appears to never have been successfully committed is this an error or
		// should recovery proceed with a new pager instance?

		// If there are at least 2 pages then try to recover the existing file
		if (exists && fileSize >= (self->smallestPhysicalBlock * 2)) {
			debug_printf("DWALPager(%s) recovering using existing file\n");

			state bool recoveredHeader = false;

			// Read physical page 0 directly
			wait(store(self->headerPage, self->readHeaderPage(self, 0)));

			// If the checksum fails for the header page, try to recover committed header backup from page 1
			if (!self->headerPage.castTo<Page>()->verifyChecksum(0)) {
				TraceEvent(SevWarn, "DWALPagerRecoveringHeader").detail("Filename", self->filename);

				wait(store(self->headerPage, self->readHeaderPage(self, 1)));

				if (!self->headerPage.castTo<Page>()->verifyChecksum(1)) {
					if (g_network->isSimulated()) {
						// TODO: Detect if process is being restarted and only throw injected if so?
						throw io_error().asInjectedFault();
					}

					Error e = checksum_failed();
					TraceEvent(SevError, "DWALPagerRecoveryFailed").detail("Filename", self->filename).error(e);
					throw e;
				}
				recoveredHeader = true;
			}

			self->pHeader = (Header*)self->headerPage->begin();

			if (self->pHeader->formatVersion != Header::FORMAT_VERSION) {
				Error e = internal_error(); // TODO:  Something better?
				TraceEvent(SevError, "DWALPagerRecoveryFailedWrongVersion")
				    .detail("Filename", self->filename)
				    .detail("Version", self->pHeader->formatVersion)
				    .detail("ExpectedVersion", Header::FORMAT_VERSION)
				    .error(e);
				throw e;
			}

			self->setPageSize(self->pHeader->pageSize);
			if (self->logicalPageSize != self->desiredPageSize) {
				TraceEvent(SevWarn, "DWALPagerPageSizeNotDesired")
				    .detail("Filename", self->filename)
				    .detail("ExistingPageSize", self->logicalPageSize)
				    .detail("DesiredPageSize", self->desiredPageSize);
			}

			self->freeList.recover(self, self->pHeader->freeList, "FreeListRecovered");
			self->delayedFreeList.recover(self, self->pHeader->delayedFreeList, "DelayedFreeListRecovered");
			self->remapQueue.recover(self, self->pHeader->remapQueue, "RemapQueueRecovered");

			Standalone<VectorRef<RemappedPage>> remaps = wait(self->remapQueue.peekAll());
			for (auto& r : remaps) {
				if (r.newPageID != invalidLogicalPageID) {
					self->remappedPages[r.originalPageID][r.version] = r.newPageID;
				}
			}

			// If the header was recovered from the backup at Page 1 then write and sync it to Page 0 before continuing.
			// If this fails, the backup header is still in tact for the next recovery attempt.
			if (recoveredHeader) {
				// Write the header to page 0
				wait(self->writeHeaderPage(0, self->headerPage));

				// Wait for all outstanding writes to complete
				wait(self->operations.signalAndCollapse());

				// Sync header
				wait(self->pageFile->sync());
				debug_printf("DWALPager(%s) Header recovery complete.\n", self->filename.c_str());
			}

			// Update the last committed header with the one that was recovered (which is the last known committed
			// header)
			self->updateCommittedHeader();
			self->addLatestSnapshot();
		} else {
			// Note: If the file contains less than 2 pages but more than 0 bytes then the pager was never successfully
			// committed. A new pager will be created in its place.
			// TODO:  Is the right behavior?

			debug_printf("DWALPager(%s) creating new pager\n");

			self->headerPage = self->newPageBuffer();
			self->pHeader = (Header*)self->headerPage->begin();

			// Now that the header page has been allocated, set page size to desired
			self->setPageSize(self->desiredPageSize);

			// Write new header using desiredPageSize
			self->pHeader->formatVersion = Header::FORMAT_VERSION;
			self->pHeader->committedVersion = 1;
			self->pHeader->oldestVersion = 1;
			// No meta key until a user sets one and commits
			self->pHeader->setMetaKey(Key());

			// There are 2 reserved pages:
			//   Page 0 - header
			//   Page 1 - header backup
			self->pHeader->pageCount = 2;

			// Create queues
			self->freeList.create(self, self->newLastPageID(), "FreeList");
			self->delayedFreeList.create(self, self->newLastPageID(), "delayedFreeList");
			self->remapQueue.create(self, self->newLastPageID(), "remapQueue");

			// The first commit() below will flush the queues and update the queue states in the header,
			// but since the queues will not be used between now and then their states will not change.
			// In order to populate lastCommittedHeader, update the header now with the queue states.
			self->pHeader->freeList = self->freeList.getState();
			self->pHeader->delayedFreeList = self->delayedFreeList.getState();
			self->pHeader->remapQueue = self->remapQueue.getState();

			// Set remaining header bytes to \xff
			memset(self->headerPage->mutate() + self->pHeader->size(), 0xff,
			       self->headerPage->size() - self->pHeader->size());

			// Since there is no previously committed header use the initial header for the initial commit.
			self->updateCommittedHeader();

			wait(self->commit());
		}

		debug_printf("DWALPager(%s) recovered.  committedVersion=%" PRId64 " logicalPageSize=%d physicalPageSize=%d\n",
		             self->filename.c_str(), self->pHeader->committedVersion, self->logicalPageSize,
		             self->physicalPageSize);
		return Void();
	}

	Reference<IPage> newPageBuffer() override {
		return Reference<IPage>(new FastAllocatedPage(logicalPageSize, physicalPageSize));
	}

	// Returns the usable size of pages returned by the pager (i.e. the size of the page that isn't pager overhead).
	// For a given pager instance, separate calls to this function must return the same value.
	int getUsablePageSize() override { return logicalPageSize - sizeof(FastAllocatedPage::Checksum); }

	// Get a new, previously available page ID.  The page will be considered in-use after the next commit
	// regardless of whether or not it was written to, until it is returned to the pager via freePage()
	ACTOR static Future<LogicalPageID> newPageID_impl(DWALPager* self) {
		// First try the free list
		Optional<LogicalPageID> freePageID = wait(self->freeList.pop());
		if (freePageID.present()) {
			debug_printf("DWALPager(%s) newPageID() returning %s from free list\n", self->filename.c_str(),
			             toString(freePageID.get()).c_str());
			return freePageID.get();
		}

		// Try to reuse pages up to the earlier of the oldest version set by the user or the oldest snapshot still in
		// the snapshots list
		ASSERT(!self->snapshots.empty());
		Optional<DelayedFreePage> delayedFreePageID =
		    wait(self->delayedFreeList.pop(DelayedFreePage{ self->effectiveOldestVersion(), 0 }));
		if (delayedFreePageID.present()) {
			debug_printf("DWALPager(%s) newPageID() returning %s from delayed free list\n", self->filename.c_str(),
			             toString(delayedFreePageID.get()).c_str());
			return delayedFreePageID.get().pageID;
		}

		// Lastly, add a new page to the pager
		LogicalPageID id = self->newLastPageID();
		debug_printf("DWALPager(%s) newPageID() returning %s at end of file\n", self->filename.c_str(),
		             toString(id).c_str());
		return id;
	};

	// Grow the pager file by pone page and return it
	LogicalPageID newLastPageID() {
		LogicalPageID id = pHeader->pageCount;
		++pHeader->pageCount;
		return id;
	}

	Future<LogicalPageID> newPageID() override { return newPageID_impl(this); }

	Future<Void> writePhysicalPage(PhysicalPageID pageID, Reference<IPage> page, bool header = false) {
		debug_printf("DWALPager(%s) op=%s %s ptr=%p\n", filename.c_str(),
		             (header ? "writePhysicalHeader" : "writePhysical"), toString(pageID).c_str(), page->begin());

		VALGRIND_MAKE_MEM_DEFINED(page->begin(), page->size());
		((Page*)page.getPtr())->updateChecksum(pageID);

		// Note:  Not using forwardError here so a write error won't be discovered until commit time.
		int blockSize = header ? smallestPhysicalBlock : physicalPageSize;
		Future<Void> f =
		    holdWhile(page, map(pageFile->write(page->begin(), blockSize, (int64_t)pageID * blockSize), [=](Void) {
			              debug_printf("DWALPager(%s) op=%s %s ptr=%p\n", filename.c_str(),
			                           (header ? "writePhysicalHeaderComplete" : "writePhysicalComplete"),
			                           toString(pageID).c_str(), page->begin());
			              return Void();
		              }));
		operations.add(f);
		return f;
	}

	Future<Void> writeHeaderPage(PhysicalPageID pageID, Reference<IPage> page) {
		return writePhysicalPage(pageID, page, true);
	}

	void updatePage(LogicalPageID pageID, Reference<IPage> data) override {
		// Get the cache entry for this page, without counting it as a cache hit as we're replacing its contents now
		PageCacheEntry& cacheEntry = pageCache.get(pageID, true);
		debug_printf("DWALPager(%s) op=write %s cached=%d reading=%d writing=%d\n", filename.c_str(),
		             toString(pageID).c_str(), cacheEntry.initialized(),
		             cacheEntry.initialized() && cacheEntry.reading(),
		             cacheEntry.initialized() && cacheEntry.writing());

		// If the page is still being read then it's not also being written because a write places
		// the new content into readFuture when the write is launched, not when it is completed.
		// Read/write ordering is being enforced waiting readers will not see the new write.  This
		// is necessary for remap erasure to work correctly since the oldest version of a page, located
		// at the original page ID, could have a pending read when that version is expired and the write
		// of the next newest version over top of the original page begins.
		if (!cacheEntry.initialized()) {
			cacheEntry.writeFuture = writePhysicalPage(pageID, data);
		} else if (cacheEntry.reading()) {
			// Wait for the read to finish, then start the write.
			cacheEntry.writeFuture = map(success(cacheEntry.readFuture), [=](Void) {
				writePhysicalPage(pageID, data);
				return Void();
			});
		}
		// If the page is being written, wait for this write before issuing the new write to ensure the
		// writes happen in the correct order
		else if (cacheEntry.writing()) {
			cacheEntry.writeFuture = map(cacheEntry.writeFuture, [=](Void) {
				writePhysicalPage(pageID, data);
				return Void();
			});
		} else {
			cacheEntry.writeFuture = writePhysicalPage(pageID, data);
		}

		// Always update the page contents immediately regardless of what happened above.
		cacheEntry.readFuture = data;
	}

	Future<LogicalPageID> atomicUpdatePage(LogicalPageID pageID, Reference<IPage> data, Version v) override {
		debug_printf("DWALPager(%s) op=writeAtomic %s @%" PRId64 "\n", filename.c_str(), toString(pageID).c_str(), v);
		// This pager does not support atomic update, so it always allocates and uses a new pageID
		Future<LogicalPageID> f = map(newPageID(), [=](LogicalPageID newPageID) {
			updatePage(newPageID, data);
			// TODO:  Possibly limit size of remap queue since it must be recovered on cold start
			RemappedPage r{ v, pageID, newPageID };
			remapQueue.pushBack(r);
			remappedPages[pageID][v] = newPageID;
			debug_printf("DWALPager(%s) pushed %s\n", filename.c_str(), RemappedPage(r).toString().c_str());
			return pageID;
		});

		// No need for forwardError here because newPageID() is already wrapped in forwardError
		return f;
	}

	void freePage(LogicalPageID pageID, Version v) override {
		// If pageID has been remapped, then it can't be freed until all existing remaps for that page have been undone,
		// so queue it for later deletion
		if (remappedPages.find(pageID) != remappedPages.end()) {
			debug_printf("DWALPager(%s) op=freeRemapped %s @%" PRId64 " oldestVersion=%" PRId64 "\n", filename.c_str(),
			             toString(pageID).c_str(), v, pLastCommittedHeader->oldestVersion);
			remapQueue.pushBack(RemappedPage{ v, pageID, invalidLogicalPageID });
			return;
		}

		// If v is older than the oldest version still readable then mark pageID as free as of the next commit
		if (v < effectiveOldestVersion()) {
			debug_printf("DWALPager(%s) op=freeNow %s @%" PRId64 " oldestVersion=%" PRId64 "\n", filename.c_str(),
			             toString(pageID).c_str(), v, pLastCommittedHeader->oldestVersion);
			freeList.pushBack(pageID);
		} else {
			// Otherwise add it to the delayed free list
			debug_printf("DWALPager(%s) op=freeLater %s @%" PRId64 " oldestVersion=%" PRId64 "\n", filename.c_str(),
			             toString(pageID).c_str(), v, pLastCommittedHeader->oldestVersion);
			delayedFreeList.pushBack({ v, pageID });
		}
	};

	// Read a physical page from the page file.  Note that header pages use a page size of smallestPhysicalBlock
	// If the user chosen physical page size is larger, then there will be a gap of unused space after the header pages
	// and before the user-chosen sized pages.
	ACTOR static Future<Reference<IPage>> readPhysicalPage(DWALPager* self, PhysicalPageID pageID,
	                                                       bool header = false) {
		if (g_network->getCurrentTask() > TaskPriority::DiskRead) {
			wait(delay(0, TaskPriority::DiskRead));
		}

		state Reference<IPage> page =
		    header ? Reference<IPage>(new FastAllocatedPage(smallestPhysicalBlock, smallestPhysicalBlock))
		           : self->newPageBuffer();
		debug_printf("DWALPager(%s) op=readPhysicalStart %s ptr=%p\n", self->filename.c_str(), toString(pageID).c_str(),
		             page->begin());

		int blockSize = header ? smallestPhysicalBlock : self->physicalPageSize;
		// TODO:  Could a dispatched read try to write to page after it has been destroyed if this actor is cancelled?
		int readBytes = wait(self->pageFile->read(page->mutate(), blockSize, (int64_t)pageID * blockSize));
		debug_printf("DWALPager(%s) op=readPhysicalComplete %s ptr=%p bytes=%d\n", self->filename.c_str(),
		             toString(pageID).c_str(), page->begin(), readBytes);

		// Header reads are checked explicitly during recovery
		if (!header) {
			Page* p = (Page*)page.getPtr();
			if (!p->verifyChecksum(pageID)) {
				debug_printf("DWALPager(%s) checksum failed for %s\n", self->filename.c_str(),
				             toString(pageID).c_str());
				Error e = checksum_failed();
				TraceEvent(SevError, "DWALPagerChecksumFailed")
				    .detail("Filename", self->filename.c_str())
				    .detail("PageID", pageID)
				    .detail("PageSize", self->physicalPageSize)
				    .detail("Offset", pageID * self->physicalPageSize)
				    .detail("CalculatedChecksum", p->calculateChecksum(pageID))
				    .detail("ChecksumInPage", p->getChecksum())
				    .error(e);
				throw e;
			}
		}
		return page;
	}

	static Future<Reference<IPage>> readHeaderPage(DWALPager* self, PhysicalPageID pageID) {
		return readPhysicalPage(self, pageID, true);
	}

	// Reads the most recent version of pageID either committed or written using updatePage()
	Future<Reference<IPage>> readPage(LogicalPageID pageID, bool cacheable, bool noHit = false) override {
		// Use cached page if present, without triggering a cache hit.
		// Otherwise, read the page and return it but don't add it to the cache
		if (!cacheable) {
			debug_printf("DWALPager(%s) op=readUncached %s\n", filename.c_str(), toString(pageID).c_str());
			PageCacheEntry* pCacheEntry = pageCache.getIfExists(pageID);
			if (pCacheEntry != nullptr) {
				debug_printf("DWALPager(%s) op=readUncachedHit %s\n", filename.c_str(), toString(pageID).c_str());
				return pCacheEntry->readFuture;
			}

			debug_printf("DWALPager(%s) op=readUncachedMiss %s\n", filename.c_str(), toString(pageID).c_str());
			return forwardError(readPhysicalPage(this, (PhysicalPageID)pageID), errorPromise);
		}

		PageCacheEntry& cacheEntry = pageCache.get(pageID, noHit);
		debug_printf("DWALPager(%s) op=read %s cached=%d reading=%d writing=%d noHit=%d\n", filename.c_str(),
		             toString(pageID).c_str(), cacheEntry.initialized(),
		             cacheEntry.initialized() && cacheEntry.reading(), cacheEntry.initialized() && cacheEntry.writing(),
		             noHit);

		if (!cacheEntry.initialized()) {
			debug_printf("DWALPager(%s) issuing actual read of %s\n", filename.c_str(), toString(pageID).c_str());
			cacheEntry.readFuture = readPhysicalPage(this, (PhysicalPageID)pageID);
			cacheEntry.writeFuture = Void();
		}

		cacheEntry.readFuture = forwardError(cacheEntry.readFuture, errorPromise);
		return cacheEntry.readFuture;
	}

	Future<Reference<IPage>> readPageAtVersion(LogicalPageID pageID, Version v, bool cacheable, bool noHit) {
		auto i = remappedPages.find(pageID);

		if (i != remappedPages.end()) {
			auto j = i->second.upper_bound(v);
			if (j != i->second.begin()) {
				--j;
				debug_printf("DWALPager(%s) read %s @%" PRId64 " -> %s\n", filename.c_str(), toString(pageID).c_str(),
				             v, toString(j->second).c_str());
				pageID = j->second;
			}
		} else {
			debug_printf("DWALPager(%s) read %s @%" PRId64 " (not remapped)\n", filename.c_str(),
			             toString(pageID).c_str(), v);
		}

		return readPage(pageID, cacheable, noHit);
	}

	// Get snapshot as of the most recent committed version of the pager
	Reference<IPagerSnapshot> getReadSnapshot(Version v) override;
	void addLatestSnapshot();

	// Set the pending oldest versiont to keep as of the next commit
	void setOldestVersion(Version v) override {
		ASSERT(v >= pHeader->oldestVersion);
		ASSERT(v <= pHeader->committedVersion);
		pHeader->oldestVersion = v;
		expireSnapshots(v);
	};

	// Get the oldest *readable* version, which is not the same as the oldest retained version as the version
	// returned could have been set as the oldest version in the pending commit
	Version getOldestVersion() override { return pHeader->oldestVersion; };

	// Calculate the *effective* oldest version, which can be older than the one set in the last commit since we
	// are allowing active snapshots to temporarily delay page reuse.
	Version effectiveOldestVersion() {
		return std::min(pLastCommittedHeader->oldestVersion, snapshots.front().version);
	}

	ACTOR static Future<Void> undoRemaps(DWALPager* self) {
		state RemappedPage cutoff;
		cutoff.version = self->effectiveOldestVersion();

		// TODO:  Use parallel reads
		// TODO:  One run of this actor might write to the same original page more than once, in which case just unmap
		// the latest
		loop {
			if (self->remapUndoStop) {
				break;
			}
			state Optional<RemappedPage> p = wait(self->remapQueue.pop(cutoff));
			if (!p.present()) {
				break;
			}
			debug_printf("DWALPager(%s) undoRemaps popped %s\n", self->filename.c_str(), p.get().toString().c_str());

			if (p.get().newPageID == invalidLogicalPageID) {
				debug_printf("DWALPager(%s) undoRemaps freeing %s\n", self->filename.c_str(),
				             p.get().toString().c_str());
				self->freePage(p.get().originalPageID, p.get().version);
			} else {
				// Read the data from the page that the original was mapped to
				Reference<IPage> data = wait(self->readPage(p.get().newPageID, false));

				// Write the data to the original page so it can be read using its original pageID
				self->updatePage(p.get().originalPageID, data);

				// Remove the remap from this page, deleting the entry for the pageID if its map becomes empty
				auto i = self->remappedPages.find(p.get().originalPageID);
				if (i->second.size() == 1) {
					self->remappedPages.erase(i);
				} else {
					i->second.erase(p.get().version);
				}

				// Now that the remap has been undone nothing will read this page so it can be freed as of the next
				// commit.
				self->freePage(p.get().newPageID, 0);
			}
		}

		debug_printf("DWALPager(%s) undoRemaps stopped, remapQueue size is %d\n", self->filename.c_str(),
		             self->remapQueue.numEntries);
		return Void();
	}

	// Flush all queues so they have no operations pending.
	ACTOR static Future<Void> flushQueues(DWALPager* self) {
		ASSERT(self->remapUndoFuture.isReady());

		// Flush remap queue separately, it's not involved in free page management
		wait(self->remapQueue.flush());

		// Flush the free list and delayed free list queues together as they are used by freePage() and newPageID()
		loop {
			state bool freeBusy = wait(self->freeList.preFlush());
			state bool delayedFreeBusy = wait(self->delayedFreeList.preFlush());

			// Once preFlush() returns false for both queues then there are no more operations pending
			// on either queue.  If preFlush() returns true for either queue in one loop execution then
			// it could have generated new work for itself or the other queue.
			if (!freeBusy && !delayedFreeBusy) {
				break;
			}
		}
		self->freeList.finishFlush();
		self->delayedFreeList.finishFlush();

		return Void();
	}

	ACTOR static Future<Void> commit_impl(DWALPager* self) {
		debug_printf("DWALPager(%s) commit begin\n", self->filename.c_str());

		// Write old committed header to Page 1
		self->writeHeaderPage(1, self->lastCommittedHeaderPage);

		// Trigger the remap eraser to stop and then wait for it.
		self->remapUndoStop = true;
		wait(self->remapUndoFuture);

		wait(flushQueues(self));

		self->pHeader->remapQueue = self->remapQueue.getState();
		self->pHeader->freeList = self->freeList.getState();
		self->pHeader->delayedFreeList = self->delayedFreeList.getState();

		// Wait for all outstanding writes to complete
		debug_printf("DWALPager(%s) waiting for outstanding writes\n", self->filename.c_str());
		wait(self->operations.signalAndCollapse());
		debug_printf("DWALPager(%s) Syncing\n", self->filename.c_str());

		// Sync everything except the header
		if (g_network->getCurrentTask() > TaskPriority::DiskWrite) {
			wait(delay(0, TaskPriority::DiskWrite));
		}
		wait(self->pageFile->sync());
		debug_printf("DWALPager(%s) commit version %" PRId64 " sync 1\n", self->filename.c_str(),
		             self->pHeader->committedVersion);

		// Update header on disk and sync again.
		wait(self->writeHeaderPage(0, self->headerPage));
		if (g_network->getCurrentTask() > TaskPriority::DiskWrite) {
			wait(delay(0, TaskPriority::DiskWrite));
		}
		wait(self->pageFile->sync());
		debug_printf("DWALPager(%s) commit version %" PRId64 " sync 2\n", self->filename.c_str(),
		             self->pHeader->committedVersion);

		// Update the last committed header for use in the next commit.
		self->updateCommittedHeader();
		self->addLatestSnapshot();

		// Try to expire snapshots up to the oldest version, in case some were being kept around due to being in use,
		// because maybe some are no longer in use.
		self->expireSnapshots(self->pHeader->oldestVersion);

		// Start unmapping pages for expired versions
		self->remapUndoStop = false;
		self->remapUndoFuture = undoRemaps(self);

		return Void();
	}

	Future<Void> commit() override {
		// Can't have more than one commit outstanding.
		ASSERT(commitFuture.isReady());
		commitFuture = forwardError(commit_impl(this), errorPromise);
		return commitFuture;
	}

	Key getMetaKey() const override { return pHeader->getMetaKey(); }

	void setCommitVersion(Version v) override { pHeader->committedVersion = v; }

	void setMetaKey(KeyRef metaKey) override { pHeader->setMetaKey(metaKey); }

	ACTOR void shutdown(DWALPager* self, bool dispose) {
		debug_printf("DWALPager(%s) shutdown cancel recovery\n", self->filename.c_str());
		self->recoverFuture.cancel();
		debug_printf("DWALPager(%s) shutdown cancel commit\n", self->filename.c_str());
		self->commitFuture.cancel();
		debug_printf("DWALPager(%s) shutdown cancel remap\n", self->filename.c_str());
		self->remapUndoFuture.cancel();

		if (self->errorPromise.canBeSet()) {
			debug_printf("DWALPager(%s) shutdown sending error\n", self->filename.c_str());
			self->errorPromise.sendError(actor_cancelled()); // Ideally this should be shutdown_in_progress
		}

		// Must wait for pending operations to complete, canceling them can cause a crash because the underlying
		// operations may be uncancellable and depend on memory from calling scope's page reference
		debug_printf("DWALPager(%s) shutdown wait for operations\n", self->filename.c_str());
		wait(self->operations.signal());

		debug_printf("DWALPager(%s) shutdown destroy page cache\n", self->filename.c_str());
		wait(self->pageCache.clear());

		// Unreference the file and clear
		self->pageFile.clear();
		if (dispose) {
			debug_printf("DWALPager(%s) shutdown deleting file\n", self->filename.c_str());
			wait(IAsyncFileSystem::filesystem()->incrementalDeleteFile(self->filename, true));
		}

		self->closedPromise.send(Void());
		delete self;
	}

	void dispose() override { shutdown(this, true); }

	void close() override { shutdown(this, false); }

	Future<Void> getError() override { return errorPromise.getFuture(); }

	Future<Void> onClosed() override { return closedPromise.getFuture(); }

	StorageBytes getStorageBytes() override {
		ASSERT(recoverFuture.isReady());
		int64_t free;
		int64_t total;
		g_network->getDiskBytes(parentDirectory(filename), free, total);
		int64_t pagerSize = pHeader->pageCount * physicalPageSize;

		// It is not exactly known how many pages on the delayed free list are usable as of right now.  It could be
		// known, if each commit delayed entries that were freeable were shuffled from the delayed free queue to the
		// free queue, but this doesn't seem necessary.
		int64_t reusable = (freeList.numEntries + delayedFreeList.numEntries) * physicalPageSize;

		return StorageBytes(free, total, pagerSize - reusable, free + reusable);
	}

	ACTOR static Future<Void> getUserPageCount_cleanup(DWALPager* self) {
		// Wait for the remap eraser to finish all of its work (not triggering stop)
		wait(self->remapUndoFuture);

		// Flush queues so there are no pending freelist operations
		wait(flushQueues(self));

		return Void();
	}

	// Get the number of pages in use by the pager's user
	Future<int64_t> getUserPageCount() override {
		return map(getUserPageCount_cleanup(this), [=](Void) {
			int64_t userPages = pHeader->pageCount - 2 - freeList.numPages - freeList.numEntries -
			                    delayedFreeList.numPages - delayedFreeList.numEntries - remapQueue.numPages;
			debug_printf("DWALPager(%s) userPages=%" PRId64 " totalPageCount=%" PRId64 " freeQueuePages=%" PRId64
			             " freeQueueCount=%" PRId64 " delayedFreeQueuePages=%" PRId64 " delayedFreeQueueCount=%" PRId64
			             " remapQueuePages=%" PRId64 " remapQueueCount=%" PRId64 "\n",
			             filename.c_str(), userPages, pHeader->pageCount, freeList.numPages, freeList.numEntries,
			             delayedFreeList.numPages, delayedFreeList.numEntries, remapQueue.numPages,
			             remapQueue.numEntries);
			return userPages;
		});
	}

	Future<Void> init() override { return recoverFuture; }

	Version getLatestVersion() override { return pLastCommittedHeader->committedVersion; }

private:
	~DWALPager() {}

	// Try to expire snapshots up to but not including v, but do not expire any snapshots that are in use.
	void expireSnapshots(Version v);

#pragma pack(push, 1)
	// Header is the format of page 0 of the database
	struct Header {
		static constexpr int FORMAT_VERSION = 2;
		uint16_t formatVersion;
		uint32_t pageSize;
		int64_t pageCount;
		FIFOQueue<LogicalPageID>::QueueState freeList;
		FIFOQueue<DelayedFreePage>::QueueState delayedFreeList;
		FIFOQueue<RemappedPage>::QueueState remapQueue;
		Version committedVersion;
		Version oldestVersion;
		int32_t metaKeySize;

		KeyRef getMetaKey() const { return KeyRef((const uint8_t*)(this + 1), metaKeySize); }

		void setMetaKey(StringRef key) {
			ASSERT(key.size() < (smallestPhysicalBlock - sizeof(Header)));
			metaKeySize = key.size();
			if (key.size() > 0) {
				memcpy(this + 1, key.begin(), key.size());
			}
		}

		int size() const { return sizeof(Header) + metaKeySize; }

	private:
		Header();
	};
#pragma pack(pop)

	struct PageCacheEntry {
		Future<Reference<IPage>> readFuture;
		Future<Void> writeFuture;

		bool initialized() const { return readFuture.isValid(); }

		bool reading() const { return !readFuture.isReady(); }

		bool writing() const { return !writeFuture.isReady(); }

		bool evictable() const {
			// Don't evict if a page is still being read or written
			return !reading() && !writing();
		}

		Future<Void> onEvictable() const { return ready(readFuture) && writeFuture; }
	};

	// Physical page sizes will always be a multiple of 4k because AsyncFileNonDurable requires
	// this in simulation, and it also makes sense for current SSDs.
	// Allowing a smaller 'logical' page size is very useful for testing.
	static constexpr int smallestPhysicalBlock = 4096;
	int physicalPageSize;
	int logicalPageSize; // In simulation testing it can be useful to use a small logical page size

	int64_t pageCacheBytes;

	// The header will be written to / read from disk as a smallestPhysicalBlock sized chunk.
	Reference<IPage> headerPage;
	Header* pHeader;

	int desiredPageSize;

	Reference<IPage> lastCommittedHeaderPage;
	Header* pLastCommittedHeader;

	std::string filename;

	typedef ObjectCache<LogicalPageID, PageCacheEntry> PageCacheT;
	PageCacheT pageCache;

	Promise<Void> closedPromise;
	Promise<Void> errorPromise;
	Future<Void> commitFuture;
	SignalableActorCollection operations;
	Future<Void> recoverFuture;
	Future<Void> remapUndoFuture;
	bool remapUndoStop;

	Reference<IAsyncFile> pageFile;

	LogicalPageQueueT freeList;

	// The delayed free list will be approximately in Version order.
	// TODO: Make this an ordered container some day.
	DelayedFreePageQueueT delayedFreeList;

	RemapQueueT remapQueue;

	struct SnapshotEntry {
		Version version;
		Promise<Void> expired;
		Reference<DWALPagerSnapshot> snapshot;
	};

	struct SnapshotEntryLessThanVersion {
		bool operator()(Version v, const SnapshotEntry& snapshot) { return v < snapshot.version; }

		bool operator()(const SnapshotEntry& snapshot, Version v) { return snapshot.version < v; }
	};

	// TODO: Better data structure
	std::unordered_map<LogicalPageID, std::map<Version, LogicalPageID>> remappedPages;

	std::deque<SnapshotEntry> snapshots;
};

// Prevents pager from reusing freed pages from version until the snapshot is destroyed
class DWALPagerSnapshot : public IPagerSnapshot, public ReferenceCounted<DWALPagerSnapshot> {
public:
	DWALPagerSnapshot(DWALPager* pager, Key meta, Version version, Future<Void> expiredFuture)
	  : pager(pager), metaKey(meta), version(version), expired(expiredFuture) {}
	virtual ~DWALPagerSnapshot() {}

	Future<Reference<const IPage>> getPhysicalPage(LogicalPageID pageID, bool cacheable, bool noHit) override {
		if (expired.isError()) {
			throw expired.getError();
		}
		return map(pager->readPageAtVersion(pageID, version, cacheable, noHit),
		           [=](Reference<IPage> p) { return Reference<const IPage>(p); });
	}

	Key getMetaKey() const override { return metaKey; }

	Version getVersion() const override { return version; }

	void addref() override { ReferenceCounted<DWALPagerSnapshot>::addref(); }

	void delref() override { ReferenceCounted<DWALPagerSnapshot>::delref(); }

	DWALPager* pager;
	Future<Void> expired;
	Version version;
	Key metaKey;
};

void DWALPager::expireSnapshots(Version v) {
	debug_printf("DWALPager(%s) expiring snapshots through %" PRId64 " snapshot count %d\n", filename.c_str(), v,
	             (int)snapshots.size());
	while (snapshots.size() > 1 && snapshots.front().version < v && snapshots.front().snapshot->isSoleOwner()) {
		debug_printf("DWALPager(%s) expiring snapshot for %" PRId64 " soleOwner=%d\n", filename.c_str(),
		             snapshots.front().version, snapshots.front().snapshot->isSoleOwner());
		// The snapshot contract could be made such that the expired promise isn't need anymore.  In practice it
		// probably is already not needed but it will gracefully handle the case where a user begins a page read
		// with a snapshot reference, keeps the page read future, and drops the snapshot reference.
		snapshots.front().expired.sendError(transaction_too_old());
		snapshots.pop_front();
	}
}

Reference<IPagerSnapshot> DWALPager::getReadSnapshot(Version v) {
	ASSERT(!snapshots.empty());

	auto i = std::upper_bound(snapshots.begin(), snapshots.end(), v, SnapshotEntryLessThanVersion());
	if (i == snapshots.begin()) {
		throw version_invalid();
	}
	--i;
	return i->snapshot;
}

void DWALPager::addLatestSnapshot() {
	Promise<Void> expired;
	snapshots.push_back({ pLastCommittedHeader->committedVersion, expired,
	                      Reference<DWALPagerSnapshot>(new DWALPagerSnapshot(this, pLastCommittedHeader->getMetaKey(),
	                                                                         pLastCommittedHeader->committedVersion,
	                                                                         expired.getFuture())) });
}

// TODO: Move this to a flow header once it is mature.
struct SplitStringRef {
	StringRef a;
	StringRef b;

	SplitStringRef(StringRef a = StringRef(), StringRef b = StringRef()) : a(a), b(b) {}

	SplitStringRef(Arena& arena, const SplitStringRef& toCopy) : a(toStringRef(arena)), b() {}

	SplitStringRef prefix(int len) const {
		if (len <= a.size()) {
			return SplitStringRef(a.substr(0, len));
		}
		len -= a.size();
		return SplitStringRef(a, b.substr(0, len));
	}

	StringRef toStringRef(Arena& arena) const {
		StringRef c = makeString(size(), arena);
		memcpy(mutateString(c), a.begin(), a.size());
		memcpy(mutateString(c) + a.size(), b.begin(), b.size());
		return c;
	}

	Standalone<StringRef> toStringRef() const {
		Arena a;
		return Standalone<StringRef>(toStringRef(a), a);
	}

	int size() const { return a.size() + b.size(); }

	int expectedSize() const { return size(); }

	std::string toString() const { return format("%s%s", a.toString().c_str(), b.toString().c_str()); }

	std::string toHexString() const { return format("%s%s", a.toHexString().c_str(), b.toHexString().c_str()); }

	struct const_iterator {
		const uint8_t* ptr;
		const uint8_t* end;
		const uint8_t* next;

		inline bool operator==(const const_iterator& rhs) const { return ptr == rhs.ptr; }

		inline const_iterator& operator++() {
			++ptr;
			if (ptr == end) {
				ptr = next;
			}
			return *this;
		}

		inline const_iterator& operator+(int n) {
			ptr += n;
			if (ptr >= end) {
				ptr = next + (ptr - end);
			}
			return *this;
		}

		inline uint8_t operator*() const { return *ptr; }
	};

	inline const_iterator begin() const { return { a.begin(), a.end(), b.begin() }; }

	inline const_iterator end() const { return { b.end() }; }

	template <typename StringT>
	int compare(const StringT& rhs) const {
		auto j = begin();
		auto k = rhs.begin();
		auto jEnd = end();
		auto kEnd = rhs.end();

		while (j != jEnd && k != kEnd) {
			int cmp = *j - *k;
			if (cmp != 0) {
				return cmp;
			}
		}

		// If we've reached the end of *this, then values are equal if rhs is also exhausted, otherwise *this is less
		// than rhs
		if (j == jEnd) {
			return k == kEnd ? 0 : -1;
		}

		return 1;
	}
};

// A BTree "page id" is actually a list of LogicalPageID's whose contents should be concatenated together.
// NOTE: Uses host byte order
typedef VectorRef<LogicalPageID> BTreePageID;

std::string toString(BTreePageID id) {
	return std::string("BTreePageID") + toString(id.begin(), id.end());
}

#define STR(x) LiteralStringRef(x)
struct RedwoodRecordRef {
	typedef uint8_t byte;

	RedwoodRecordRef(KeyRef key = KeyRef(), Version ver = 0, Optional<ValueRef> value = {})
	  : key(key), version(ver), value(value) {}

	RedwoodRecordRef(Arena& arena, const RedwoodRecordRef& toCopy) : key(arena, toCopy.key), version(toCopy.version) {
		if (toCopy.value.present()) {
			value = ValueRef(arena, toCopy.value.get());
		}
	}

	KeyValueRef toKeyValueRef() const { return KeyValueRef(key, value.get()); }

	// RedwoodRecordRefs are used for both internal and leaf pages of the BTree.
	// Boundary records in internal pages are made from leaf records.
	// These functions make creating and working with internal page records more convenient.
	inline BTreePageID getChildPage() const {
		ASSERT(value.present());
		return BTreePageID((LogicalPageID*)value.get().begin(), value.get().size() / sizeof(LogicalPageID));
	}

	inline void setChildPage(BTreePageID id) {
		value = ValueRef((const uint8_t*)id.begin(), id.size() * sizeof(LogicalPageID));
	}

	inline void setChildPage(Arena& arena, BTreePageID id) {
		value = ValueRef(arena, (const uint8_t*)id.begin(), id.size() * sizeof(LogicalPageID));
	}

	inline RedwoodRecordRef withPageID(BTreePageID id) const {
		return RedwoodRecordRef(key, version, ValueRef((const uint8_t*)id.begin(), id.size() * sizeof(LogicalPageID)));
	}

	inline RedwoodRecordRef withoutValue() const { return RedwoodRecordRef(key, version); }

	// Truncate (key, version, part) tuple to len bytes.
	void truncate(int len) {
		ASSERT(len <= key.size());
		key = key.substr(0, len);
		version = 0;
	}

	// Find the common key prefix between two records, assuming that the first skipLen bytes are the same
	inline int getCommonPrefixLen(const RedwoodRecordRef& other, int skipLen = 0) const {
		int skipStart = std::min(skipLen, key.size());
		return skipStart + commonPrefixLength(key.begin() + skipStart, other.key.begin() + skipStart,
		                                      std::min(other.key.size(), key.size()) - skipStart);
	}

	// Compares and orders by key, version, chunk.total, chunk.start, value
	// This is the same order that delta compression uses for prefix borrowing
	int compare(const RedwoodRecordRef& rhs, int skip = 0) const {
		int keySkip = std::min(skip, key.size());
		int cmp = key.substr(keySkip).compare(rhs.key.substr(keySkip));

		if (cmp == 0) {
			cmp = version - rhs.version;
			if (cmp == 0) {
				cmp = value.compare(rhs.value);
			}
		}
		return cmp;
	}

	bool sameUserKey(const StringRef& k, int skipLen) const {
		// Keys are the same if the sizes are the same and either the skipLen is longer or the non-skipped suffixes are
		// the same.
		return (key.size() == k.size()) && (key.substr(skipLen) == k.substr(skipLen));
	}

	bool sameExceptValue(const RedwoodRecordRef& rhs, int skipLen = 0) const {
		return sameUserKey(rhs.key, skipLen) && version == rhs.version;
	}

	// TODO: Use SplitStringRef (unless it ends up being slower)
	KeyRef key;
	Optional<ValueRef> value;
	Version version;

	int expectedSize() const { return key.expectedSize() + value.expectedSize(); }

	class Reader {
	public:
		Reader(const void* ptr) : rptr((const byte*)ptr) {}

		const byte* rptr;

		StringRef readString(int len) {
			StringRef s(rptr, len);
			rptr += len;
			return s;
		}
	};

#pragma pack(push, 1)
	struct Delta {

		uint8_t flags;

		// Four field sizing schemes ranging from 3 to 8 bytes, with 3 being the most common.
		union {
			struct {
				uint8_t prefixLength;
				uint8_t suffixLength;
				uint8_t valueLength;
			} LengthFormat0;

			struct {
				uint8_t prefixLength;
				uint8_t suffixLength;
				uint16_t valueLength;
			} LengthFormat1;

			struct {
				uint8_t prefixLength;
				uint8_t suffixLength;
				uint32_t valueLength;
			} LengthFormat2;

			struct {
				uint16_t prefixLength;
				uint16_t suffixLength;
				uint32_t valueLength;
			} LengthFormat3;
		};

		struct int48_t {
			static constexpr int64_t MASK = 0xFFFFFFFFFFFFLL;
			int32_t high;
			int16_t low;
		};

		static constexpr int LengthFormatSizes[] = { sizeof(LengthFormat0), sizeof(LengthFormat1),
			                                         sizeof(LengthFormat2), sizeof(LengthFormat3) };
		static constexpr int VersionDeltaSizes[] = { 0, sizeof(int32_t), sizeof(int48_t), sizeof(int64_t) };

		// Serialized Format
		//
		// Flags - 1 byte
		//    1 bit - borrow source is prev ancestor (otherwise next ancestor)
		//    1 bit - item is deleted
		//    1 bit - has value (different from zero-length value, if 0 value len will be 0)
		//    1 bits - has nonzero version
		//    2 bits - version delta integer size code, maps to 0, 4, 6, 8
		//    2 bits - length fields format
		//
		// Length fields using 3 to 8 bytes total depending on length fields format
		//
		// Byte strings
		//    Key suffix bytes
		//    Value bytes
		//    Version delta bytes
		//

		enum EFlags {
			PREFIX_SOURCE_PREV = 0x80,
			IS_DELETED = 0x40,
			HAS_VALUE = 0x20,
			HAS_VERSION = 0x10,
			VERSION_DELTA_SIZE = 0xC,
			LENGTHS_FORMAT = 0x03
		};

		static inline int determineLengthFormat(int prefixLength, int suffixLength, int valueLength) {
			// Large prefix or suffix length, which should be rare, is format 3
			if (prefixLength > 0xFF || suffixLength > 0xFF) {
				return 3;
			} else if (valueLength < 0x100) {
				return 0;
			} else if (valueLength < 0x10000) {
				return 1;
			} else {
				return 2;
			}
		}

		// Large prefix or suffix length, which should be rare, is format 3
		byte* data() const {
			switch (flags & LENGTHS_FORMAT) {
			case 0:
				return (byte*)(&LengthFormat0 + 1);
			case 1:
				return (byte*)(&LengthFormat1 + 1);
			case 2:
				return (byte*)(&LengthFormat2 + 1);
			case 3:
			default:
				return (byte*)(&LengthFormat3 + 1);
			}
		}

		int getKeyPrefixLength() const {
			switch (flags & LENGTHS_FORMAT) {
			case 0:
				return LengthFormat0.prefixLength;
			case 1:
				return LengthFormat1.prefixLength;
			case 2:
				return LengthFormat2.prefixLength;
			case 3:
			default:
				return LengthFormat3.prefixLength;
			}
		}

		int getKeySuffixLength() const {
			switch (flags & LENGTHS_FORMAT) {
			case 0:
				return LengthFormat0.suffixLength;
			case 1:
				return LengthFormat1.suffixLength;
			case 2:
				return LengthFormat2.suffixLength;
			case 3:
			default:
				return LengthFormat3.suffixLength;
			}
		}

		int getValueLength() const {
			switch (flags & LENGTHS_FORMAT) {
			case 0:
				return LengthFormat0.valueLength;
			case 1:
				return LengthFormat1.valueLength;
			case 2:
				return LengthFormat2.valueLength;
			case 3:
			default:
				return LengthFormat3.valueLength;
			}
		}

		StringRef getKeySuffix() const { return StringRef(data(), getKeySuffixLength()); }

		StringRef getValue() const { return StringRef(data() + getKeySuffixLength(), getValueLength()); }

		bool hasVersion() const { return flags & HAS_VERSION; }

		int getVersionDeltaSizeBytes() const {
			int code = (flags & VERSION_DELTA_SIZE) >> 2;
			return VersionDeltaSizes[code];
		}

		static int getVersionDeltaSizeBytes(Version d) {
			if (d == 0) {
				return 0;
			} else if (d == (int32_t)d) {
				return sizeof(int32_t);
			} else if (d == (d & int48_t::MASK)) {
				return sizeof(int48_t);
			}
			return sizeof(int64_t);
		}

		int getVersionDelta(const uint8_t* r) const {
			int code = (flags & VERSION_DELTA_SIZE) >> 2;
			switch (code) {
			case 0:
				return 0;
			case 1:
				return *(int32_t*)r;
			case 2:
				return (((int64_t)((int48_t*)r)->high) << 16) | (((int48_t*)r)->low & 0xFFFF);
			case 3:
			default:
				return *(int64_t*)r;
			}
		}

		// Version delta size should be 0 before calling
		int setVersionDelta(Version d, uint8_t* w) {
			flags |= HAS_VERSION;
			if (d == 0) {
				return 0;
			} else if (d == (int32_t)d) {
				flags |= 1 << 2;
				*(uint32_t*)w = d;
				return sizeof(uint32_t);
			} else if (d == (d & int48_t::MASK)) {
				flags |= 2 << 2;
				((int48_t*)w)->high = d >> 16;
				((int48_t*)w)->low = d;
				return sizeof(int48_t);
			} else {
				flags |= 3 << 2;
				*(int64_t*)w = d;
				return sizeof(int64_t);
			}
		}

		bool hasValue() const { return flags & HAS_VALUE; }

		void setPrefixSource(bool val) {
			if (val) {
				flags |= PREFIX_SOURCE_PREV;
			} else {
				flags &= ~PREFIX_SOURCE_PREV;
			}
		}

		bool getPrefixSource() const { return flags & PREFIX_SOURCE_PREV; }

		void setDeleted(bool val) {
			if (val) {
				flags |= IS_DELETED;
			} else {
				flags &= ~IS_DELETED;
			}
		}

		bool getDeleted() const { return flags & IS_DELETED; }

		RedwoodRecordRef apply(const RedwoodRecordRef& base, Arena& arena) const {
			int keyPrefixLen = getKeyPrefixLength();
			int keySuffixLen = getKeySuffixLength();
			int valueLen = hasValue() ? getValueLength() : 0;

			StringRef k;

			Reader r(data());
			// If there is a key suffix, reconstitute the complete key into a contiguous string
			if (keySuffixLen > 0) {
				StringRef keySuffix = r.readString(keySuffixLen);
				k = makeString(keyPrefixLen + keySuffixLen, arena);
				memcpy(mutateString(k), base.key.begin(), keyPrefixLen);
				memcpy(mutateString(k) + keyPrefixLen, keySuffix.begin(), keySuffixLen);
			} else {
				// Otherwise just reference the base key's memory
				k = base.key.substr(0, keyPrefixLen);
			}

			Optional<ValueRef> value;
			if (hasValue()) {
				value = r.readString(valueLen);
			}

			Version v = 0;
			if (hasVersion()) {
				v = base.version + getVersionDelta(r.rptr);
			}

			return RedwoodRecordRef(k, v, value);
		}

		int size() const {
			int size = 1 + getVersionDeltaSizeBytes();
			switch (flags & LENGTHS_FORMAT) {
			case 0:
				return size + sizeof(LengthFormat0) + LengthFormat0.suffixLength + LengthFormat0.valueLength;
			case 1:
				return size + sizeof(LengthFormat1) + LengthFormat1.suffixLength + LengthFormat1.valueLength;
			case 2:
				return size + sizeof(LengthFormat2) + LengthFormat2.suffixLength + LengthFormat2.valueLength;
			case 3:
			default:
				return size + sizeof(LengthFormat3) + LengthFormat3.suffixLength + LengthFormat3.valueLength;
			}
		}

		std::string toString() const {
			std::string flagString = " ";
			if (flags & PREFIX_SOURCE_PREV) {
				flagString += "PrefixSource|";
			}
			if (flags & IS_DELETED) {
				flagString += "IsDeleted|";
			}
			if (hasValue()) {
				flagString += "HasValue|";
			}
			if (hasVersion()) {
				flagString += "HasVersion|";
			}
			int lengthFormat = flags & LENGTHS_FORMAT;

			Reader r(data());
			int prefixLen = getKeyPrefixLength();
			int keySuffixLen = getKeySuffixLength();
			int valueLen = getValueLength();

			return format("lengthFormat: %d  totalDeltaSize: %d  flags: %s  prefixLen: %d  keySuffixLen: %d  "
			              "versionDeltaSizeBytes: %d  valueLen %d  raw: %s",
			              lengthFormat, size(), flagString.c_str(), prefixLen, keySuffixLen, getVersionDeltaSizeBytes(),
			              valueLen, StringRef((const uint8_t*)this, size()).toHexString().c_str());
		}
	};

	// Using this class as an alternative for Delta enables reading a DeltaTree<RecordRef> while only decoding
	// its values, so the Reader does not require the original prev/next ancestors.
	struct DeltaValueOnly : Delta {
		RedwoodRecordRef apply(const RedwoodRecordRef& base, Arena& arena) const {
			Optional<ValueRef> value;

			if (hasValue()) {
				value = getValue();
			}

			return RedwoodRecordRef(StringRef(), 0, value);
		}
	};
#pragma pack(pop)

	bool operator==(const RedwoodRecordRef& rhs) const { return compare(rhs) == 0; }

	bool operator!=(const RedwoodRecordRef& rhs) const { return compare(rhs) != 0; }

	bool operator<(const RedwoodRecordRef& rhs) const { return compare(rhs) < 0; }

	bool operator>(const RedwoodRecordRef& rhs) const { return compare(rhs) > 0; }

	bool operator<=(const RedwoodRecordRef& rhs) const { return compare(rhs) <= 0; }

	bool operator>=(const RedwoodRecordRef& rhs) const { return compare(rhs) >= 0; }

	// Worst case overhead means to assu
	int deltaSize(const RedwoodRecordRef& base, int skipLen, bool worstCaseOverhead) const {
		int prefixLen = getCommonPrefixLen(base, skipLen);
		int keySuffixLen = key.size() - prefixLen;
		int valueLen = value.present() ? value.get().size() : 0;

		int formatType;
		int versionBytes;
		if (worstCaseOverhead) {
			formatType = Delta::determineLengthFormat(key.size(), key.size(), valueLen);
			versionBytes = version == 0 ? 0 : Delta::getVersionDeltaSizeBytes(version << 1);
		} else {
			formatType = Delta::determineLengthFormat(prefixLen, keySuffixLen, valueLen);
			versionBytes = version == 0 ? 0 : Delta::getVersionDeltaSizeBytes(version - base.version);
		}

		return 1 + Delta::LengthFormatSizes[formatType] + keySuffixLen + valueLen + versionBytes;
	}

	// commonPrefix between *this and base can be passed if known
	int writeDelta(Delta& d, const RedwoodRecordRef& base, int keyPrefixLen = -1) const {
		d.flags = value.present() ? Delta::HAS_VALUE : 0;

		if (keyPrefixLen < 0) {
			keyPrefixLen = getCommonPrefixLen(base, 0);
		}

		StringRef keySuffix = key.substr(keyPrefixLen);
		int valueLen = value.present() ? value.get().size() : 0;

		int formatType = Delta::determineLengthFormat(keyPrefixLen, keySuffix.size(), valueLen);
		d.flags |= formatType;

		switch (formatType) {
		case 0:
			d.LengthFormat0.prefixLength = keyPrefixLen;
			d.LengthFormat0.suffixLength = keySuffix.size();
			d.LengthFormat0.valueLength = valueLen;
			break;
		case 1:
			d.LengthFormat1.prefixLength = keyPrefixLen;
			d.LengthFormat1.suffixLength = keySuffix.size();
			d.LengthFormat1.valueLength = valueLen;
			break;
		case 2:
			d.LengthFormat2.prefixLength = keyPrefixLen;
			d.LengthFormat2.suffixLength = keySuffix.size();
			d.LengthFormat2.valueLength = valueLen;
			break;
		case 3:
		default:
			d.LengthFormat3.prefixLength = keyPrefixLen;
			d.LengthFormat3.suffixLength = keySuffix.size();
			d.LengthFormat3.valueLength = valueLen;
			break;
		}

		uint8_t* wptr = d.data();
		// Write key suffix string
		wptr = keySuffix.copyTo(wptr);

		// Write value bytes
		if (value.present()) {
			wptr = value.get().copyTo(wptr);
		}

		if (version != 0) {
			wptr += d.setVersionDelta(version - base.version, wptr);
		}

		return wptr - (uint8_t*)&d;
	}

	static std::string kvformat(StringRef s, int hexLimit = -1) {
		bool hex = false;

		for (auto c : s) {
			if (!isprint(c)) {
				hex = true;
				break;
			}
		}

		return hex ? s.toHexString(hexLimit) : s.toString();
	}

	std::string toString(bool leaf = true) const {
		std::string r;
		r += format("'%s'@%" PRId64 " => ", kvformat(key).c_str(), version);
		if (value.present()) {
			if (leaf) {
				r += format("'%s'", kvformat(value.get()).c_str());
			} else {
				r += format("[%s]", ::toString(getChildPage()).c_str());
			}
		} else {
			r += "(absent)";
		}
		return r;
	}
};

struct BTreePage {
	typedef DeltaTree<RedwoodRecordRef> BinaryTree;
	typedef DeltaTree<RedwoodRecordRef, RedwoodRecordRef::DeltaValueOnly> ValueTree;

#pragma pack(push, 1)
	struct {
		uint8_t height;
		uint32_t kvBytes;
	};
#pragma pack(pop)

	int size() const {
		const BinaryTree* t = &tree();
		return (uint8_t*)t - (uint8_t*)this + t->size();
	}

	bool isLeaf() const { return height == 1; }

	BinaryTree& tree() { return *(BinaryTree*)(this + 1); }

	const BinaryTree& tree() const { return *(const BinaryTree*)(this + 1); }

	const ValueTree& valueTree() const { return *(const ValueTree*)(this + 1); }

	std::string toString(bool write, BTreePageID id, Version ver, const RedwoodRecordRef* lowerBound,
	                     const RedwoodRecordRef* upperBound) const {
		std::string r;
		r += format("BTreePage op=%s %s @%" PRId64
		            " ptr=%p height=%d count=%d kvBytes=%d\n  lowerBound: %s\n  upperBound: %s\n",
		            write ? "write" : "read", ::toString(id).c_str(), ver, this, height, (int)tree().numItems,
		            (int)kvBytes, lowerBound->toString(false).c_str(), upperBound->toString(false).c_str());
		try {
			if (tree().numItems > 0) {
				// This doesn't use the cached reader for the page but it is only for debugging purposes
				BinaryTree::Mirror reader(&tree(), lowerBound, upperBound);
				BinaryTree::Cursor c = reader.getCursor();

				c.moveFirst();
				ASSERT(c.valid());

				bool anyOutOfRange = false;
				do {
					r += "  ";
					r += c.get().toString(height == 1);

					bool tooLow = c.get().withoutValue() < lowerBound->withoutValue();
					bool tooHigh = c.get().withoutValue() >= upperBound->withoutValue();
					if (tooLow || tooHigh) {
						anyOutOfRange = true;
						if (tooLow) {
							r += " (too low)";
						}
						if (tooHigh) {
							r += " (too high)";
						}
					}
					r += "\n";

				} while (c.moveNext());
				ASSERT(!anyOutOfRange);
			}
		} catch (Error& e) {
			debug_printf("BTreePage::toString ERROR: %s\n", e.what());
			debug_printf("BTreePage::toString partial result: %s\n", r.c_str());
			throw;
		}

		return r;
	}
};

static void makeEmptyRoot(Reference<IPage> page) {
	BTreePage* btpage = (BTreePage*)page->begin();
	btpage->height = 1;
	btpage->kvBytes = 0;
	btpage->tree().build(page->size(), nullptr, nullptr, nullptr, nullptr);
}

BTreePage::BinaryTree::Cursor getCursor(const Reference<const IPage>& page) {
	return ((BTreePage::BinaryTree::Mirror*)page->userData)->getCursor();
}

struct BoundaryRefAndPage {
	Standalone<RedwoodRecordRef> lowerBound;
	Reference<IPage> firstPage;
	std::vector<Reference<IPage>> extPages;

	std::string toString() const {
		return format("[%s, %d pages]", lowerBound.toString().c_str(), extPages.size() + (firstPage ? 1 : 0));
	}
};

#define NOT_IMPLEMENTED                                                                                                \
	{ UNSTOPPABLE_ASSERT(false); }

#pragma pack(push, 1)
template <typename T, typename SizeT = int8_t>
struct InPlaceArray {
	SizeT count;

	const T* begin() const { return (T*)(this + 1); }

	T* begin() { return (T*)(this + 1); }

	const T* end() const { return begin() + count; }

	T* end() { return begin() + count; }

	VectorRef<T> get() { return VectorRef<T>(begin(), count); }

	void set(VectorRef<T> v, int availableSpace) {
		ASSERT(sizeof(T) * v.size() <= availableSpace);
		count = v.size();
		memcpy(begin(), v.begin(), sizeof(T) * v.size());
	}

	int extraSize() const { return count * sizeof(T); }
};
#pragma pack(pop)

class VersionedBTree : public IVersionedStore {
public:
	// The first possible internal record possible in the tree
	static RedwoodRecordRef dbBegin;
	// A record which is greater than the last possible record in the tree
	static RedwoodRecordRef dbEnd;

	struct LazyDeleteQueueEntry {
		Version version;
		Standalone<BTreePageID> pageID;

		bool operator<(const LazyDeleteQueueEntry& rhs) const { return version < rhs.version; }

		int readFromBytes(const uint8_t* src) {
			version = *(Version*)src;
			src += sizeof(Version);
			int count = *src++;
			pageID = BTreePageID((LogicalPageID*)src, count);
			return bytesNeeded();
		}

		int bytesNeeded() const { return sizeof(Version) + 1 + (pageID.size() * sizeof(LogicalPageID)); }

		int writeToBytes(uint8_t* dst) const {
			*(Version*)dst = version;
			dst += sizeof(Version);
			*dst++ = pageID.size();
			memcpy(dst, pageID.begin(), pageID.size() * sizeof(LogicalPageID));
			return bytesNeeded();
		}

		std::string toString() const { return format("{%s @%" PRId64 "}", ::toString(pageID).c_str(), version); }
	};

	typedef FIFOQueue<LazyDeleteQueueEntry> LazyDeleteQueueT;

#pragma pack(push, 1)
	struct MetaKey {
		static constexpr int FORMAT_VERSION = 7;
		// This serves as the format version for the entire tree, individual pages will not be versioned
		uint16_t formatVersion;
		uint8_t height;
		LazyDeleteQueueT::QueueState lazyDeleteQueue;
		InPlaceArray<LogicalPageID> root;

		KeyRef asKeyRef() const { return KeyRef((uint8_t*)this, sizeof(MetaKey) + root.extraSize()); }

		void fromKeyRef(KeyRef k) {
			memcpy(this, k.begin(), k.size());
			ASSERT(formatVersion == FORMAT_VERSION);
		}

		std::string toString() {
			return format("{height=%d  formatVersion=%d  root=%s  lazyDeleteQueue=%s}", (int)height, (int)formatVersion,
			              ::toString(root.get()).c_str(), lazyDeleteQueue.toString().c_str());
		}
	};
#pragma pack(pop)

	struct Counts {
		Counts() {
			memset(this, 0, sizeof(Counts));
			startTime = g_network ? now() : 0;
		}

		void clear() { *this = Counts(); }

		int64_t pageReads;
		int64_t extPageReads;
		int64_t pagePreloads;
		int64_t extPagePreloads;
		int64_t setBytes;
		int64_t pageWrites;
		int64_t extPageWrites;
		int64_t sets;
		int64_t clears;
		int64_t clearSingleKey;
		int64_t commits;
		int64_t gets;
		int64_t getRanges;
		int64_t commitToPage;
		int64_t commitToPageStart;
		int64_t pageUpdates;
		double startTime;

		std::string toString(bool clearAfter = false) {
			const char* labels[] = { "set",          "clear",           "clearSingleKey", "get",
				                     "getRange",     "commit",          "pageReads",      "extPageRead",
				                     "pagePreloads", "extPagePreloads", "pageWrite",      "extPageWrite",
				                     "commitPage",   "commitPageStart", "pageUpdates" };
			const int64_t values[] = {
				sets,         clears,       clearSingleKey,  gets,       getRanges,     commits,      pageReads,
				extPageReads, pagePreloads, extPagePreloads, pageWrites, extPageWrites, commitToPage, commitToPageStart,
				pageUpdates
			};

			double elapsed = now() - startTime;
			std::string s;
			for (int i = 0; i < sizeof(values) / sizeof(int64_t); ++i) {
				s += format("%s=%" PRId64 " (%d/s)  ", labels[i], values[i], int(values[i] / elapsed));
			}

			if (clearAfter) {
				clear();
			}

			return s;
		}
	};

	// Using a static for metrics because a single process shouldn't normally have multiple storage engines
	static Counts counts;

	// All async opts on the btree are based on pager reads, writes, and commits, so
	// we can mostly forward these next few functions to the pager
	Future<Void> getError() { return m_pager->getError(); }

	Future<Void> onClosed() { return m_pager->onClosed(); }

	void close_impl(bool dispose) {
		auto* pager = m_pager;
		delete this;
		if (dispose)
			pager->dispose();
		else
			pager->close();
	}

	void dispose() { return close_impl(true); }

	void close() { return close_impl(false); }

	KeyValueStoreType getType() NOT_IMPLEMENTED bool supportsMutation(int op) NOT_IMPLEMENTED StorageBytes
	    getStorageBytes() {
		return m_pager->getStorageBytes();
	}

	// Writes are provided in an ordered stream.
	// A write is considered part of (a change leading to) the version determined by the previous call to
	// setWriteVersion() A write shall not become durable until the following call to commit() begins, and shall be
	// durable once the following call to commit() returns
	void set(KeyValueRef keyValue) {
		++counts.sets;
		m_pBuffer->insert(keyValue.key).mutation().setBoundaryValue(m_pBuffer->copyToArena(keyValue.value));
	}

	void clear(KeyRangeRef clearedRange) {
		// Optimization for single key clears to create just one mutation boundary instead of two
		if (clearedRange.begin.size() == clearedRange.end.size() - 1 &&
		    clearedRange.end[clearedRange.end.size() - 1] == 0 && clearedRange.end.startsWith(clearedRange.begin)) {
			++counts.clears;
			++counts.clearSingleKey;
			m_pBuffer->insert(clearedRange.begin).mutation().clearBoundary();
			return;
		}

		++counts.clears;
		MutationBuffer::iterator iBegin = m_pBuffer->insert(clearedRange.begin);
		MutationBuffer::iterator iEnd = m_pBuffer->insert(clearedRange.end);

		iBegin.mutation().clearAll();
		++iBegin;
		m_pBuffer->erase(iBegin, iEnd);
	}

	void mutate(int op, StringRef param1, StringRef param2) NOT_IMPLEMENTED

	    void setOldestVersion(Version v) {
		m_newOldestVersion = v;
	}

	Version getOldestVersion() { return m_pager->getOldestVersion(); }

	Version getLatestVersion() {
		if (m_writeVersion != invalidVersion) return m_writeVersion;
		return m_pager->getLatestVersion();
	}

	Version getWriteVersion() { return m_writeVersion; }

	Version getLastCommittedVersion() { return m_lastCommittedVersion; }

	VersionedBTree(IPager2* pager, std::string name)
	  : m_pager(pager), m_writeVersion(invalidVersion), m_lastCommittedVersion(invalidVersion), m_pBuffer(nullptr),
	    m_name(name) {
		m_init = init_impl(this);
		m_latestCommit = m_init;
	}

	ACTOR static Future<int> incrementalSubtreeClear(VersionedBTree* self, bool* pStop = nullptr, int batchSize = 10,
	                                                 unsigned int minPages = 0,
	                                                 int maxPages = std::numeric_limits<int>::max()) {
		// TODO: Is it contractually okay to always to read at the latest version?
		state Reference<IPagerSnapshot> snapshot = self->m_pager->getReadSnapshot(self->m_pager->getLatestVersion());
		state int freedPages = 0;

		loop {
			state std::vector<std::pair<LazyDeleteQueueEntry, Future<Reference<const IPage>>>> entries;

			// Take up to batchSize pages from front of queue
			while (entries.size() < batchSize) {
				Optional<LazyDeleteQueueEntry> q = wait(self->m_lazyDeleteQueue.pop());
				debug_printf("LazyDelete: popped %s\n", toString(q).c_str());
				if (!q.present()) {
					break;
				}
				// Start reading the page, without caching
				entries.push_back(
				    std::make_pair(q.get(), self->readPage(snapshot, q.get().pageID, nullptr, nullptr, true)));
			}

			if (entries.empty()) {
				break;
			}

			state int i;
			for (i = 0; i < entries.size(); ++i) {
				Reference<const IPage> p = wait(entries[i].second);
				const LazyDeleteQueueEntry& entry = entries[i].first;
				const BTreePage& btPage = *(BTreePage*)p->begin();
				debug_printf("LazyDelete: processing %s\n", toString(entry).c_str());

				// Level 1 (leaf) nodes should never be in the lazy delete queue
				ASSERT(btPage.height > 1);

				// Iterate over page entries, skipping key decoding using BTreePage::ValueTree which uses
				// RedwoodRecordRef::DeltaValueOnly as the delta type type to skip key decoding
				BTreePage::ValueTree::Mirror reader(&btPage.valueTree(), &dbBegin, &dbEnd);
				auto c = reader.getCursor();
				ASSERT(c.moveFirst());
				Version v = entry.version;
				while (1) {
					if (c.get().value.present()) {
						BTreePageID btChildPageID = c.get().getChildPage();
						// If this page is height 2, then the children are leaves so free
						if (btPage.height == 2) {
							debug_printf("LazyDelete: freeing child %s\n", toString(btChildPageID).c_str());
							self->freeBtreePage(btChildPageID, v);
							freedPages += btChildPageID.size();
						} else {
							// Otherwise, queue them for lazy delete.
							debug_printf("LazyDelete: queuing child %s\n", toString(btChildPageID).c_str());
							self->m_lazyDeleteQueue.pushFront(LazyDeleteQueueEntry{ v, btChildPageID });
						}
					}
					if (!c.moveNext()) {
						break;
					}
				}

				// Free the page, now that its children have either been freed or queued
				debug_printf("LazyDelete: freeing queue entry %s\n", toString(entry.pageID).c_str());
				self->freeBtreePage(entry.pageID, v);
				freedPages += entry.pageID.size();
			}

			// If stop is set and we've freed the minimum number of pages required, or the maximum is exceeded, return.
			if ((freedPages >= minPages && pStop != nullptr && *pStop) || freedPages >= maxPages) {
				break;
			}
		}

		debug_printf("LazyDelete: freed %d pages, %s has %" PRId64 " entries\n", freedPages,
		             self->m_lazyDeleteQueue.name.c_str(), self->m_lazyDeleteQueue.numEntries);
		return freedPages;
	}

	ACTOR static Future<Void> init_impl(VersionedBTree* self) {
		wait(self->m_pager->init());

		state Version latest = self->m_pager->getLatestVersion();
		self->m_newOldestVersion = self->m_pager->getOldestVersion();

		debug_printf("Recovered pager to version %" PRId64 ", oldest version is %" PRId64 "\n",
		             self->m_newOldestVersion);

		state Key meta = self->m_pager->getMetaKey();
		if (meta.size() == 0) {
			self->m_header.formatVersion = MetaKey::FORMAT_VERSION;
			LogicalPageID id = wait(self->m_pager->newPageID());
			BTreePageID newRoot((LogicalPageID*)&id, 1);
			debug_printf("new root %s\n", toString(newRoot).c_str());
			self->m_header.root.set(newRoot, sizeof(headerSpace) - sizeof(m_header));
			self->m_header.height = 1;
			++latest;
			Reference<IPage> page = self->m_pager->newPageBuffer();
			makeEmptyRoot(page);
			self->m_pager->updatePage(id, page);
			self->m_pager->setCommitVersion(latest);

			LogicalPageID newQueuePage = wait(self->m_pager->newPageID());
			self->m_lazyDeleteQueue.create(self->m_pager, newQueuePage, "LazyDeleteQueue");
			self->m_header.lazyDeleteQueue = self->m_lazyDeleteQueue.getState();
			self->m_pager->setMetaKey(self->m_header.asKeyRef());
			wait(self->m_pager->commit());
			debug_printf("Committed initial commit.\n");
		} else {
			self->m_header.fromKeyRef(meta);
			self->m_lazyDeleteQueue.recover(self->m_pager, self->m_header.lazyDeleteQueue, "LazyDeleteQueueRecovered");
		}

		debug_printf("Recovered btree at version %" PRId64 ": %s\n", latest, self->m_header.toString().c_str());

		self->m_lastCommittedVersion = latest;
		return Void();
	}

	Future<Void> init() override { return m_init; }

	virtual ~VersionedBTree() {
		// This probably shouldn't be called directly (meaning deleting an instance directly) but it should be safe,
		// it will cancel init and commit and leave the pager alive but with potentially an incomplete set of
		// uncommitted writes so it should not be committed.
		m_init.cancel();
		m_latestCommit.cancel();
	}

	Reference<IStoreCursor> readAtVersion(Version v) {
		// Only committed versions can be read.
		ASSERT(v <= m_lastCommittedVersion);
		Reference<IPagerSnapshot> snapshot = m_pager->getReadSnapshot(v);

		// This is a ref because snapshot will continue to hold the metakey value memory
		KeyRef m = snapshot->getMetaKey();

		// Currently all internal records generated in the write path are at version 0
		return Reference<IStoreCursor>(new Cursor(snapshot, ((MetaKey*)m.begin())->root.get(), (Version)0));
	}

	// Must be nondecreasing
	void setWriteVersion(Version v) {
		ASSERT(v > m_lastCommittedVersion);
		// If there was no current mutation buffer, create one in the buffer map and update m_pBuffer
		if (m_pBuffer == nullptr) {
			// When starting a new mutation buffer its start version must be greater than the last write version
			ASSERT(v > m_writeVersion);
			m_pBuffer = &m_mutationBuffers[v];
		} else {
			// It's OK to set the write version to the same version repeatedly so long as m_pBuffer is not null
			ASSERT(v >= m_writeVersion);
		}
		m_writeVersion = v;
	}

	Future<Void> commit() {
		if (m_pBuffer == nullptr) return m_latestCommit;
		return commit_impl(this);
	}

	ACTOR static Future<Void> destroyAndCheckSanity_impl(VersionedBTree* self) {
		ASSERT(g_network->isSimulated());

		debug_printf("Clearing tree.\n");
		self->setWriteVersion(self->getLatestVersion() + 1);
		self->clear(KeyRangeRef(dbBegin.key, dbEnd.key));

		loop {
			state int freedPages = wait(self->incrementalSubtreeClear(self));
			wait(self->commit());
			// Keep looping until the last commit doesn't do anything at all
			if (self->m_lazyDeleteQueue.numEntries == 0 && freedPages == 0) {
				break;
			}
			self->setWriteVersion(self->getLatestVersion() + 1);
		}

		// Forget all but the latest version of the tree.
		debug_printf("Discarding all old versions.\n");
		self->setOldestVersion(self->getLastCommittedVersion());
		self->setWriteVersion(self->getLatestVersion() + 1);
		wait(self->commit());

		// The lazy delete queue should now be empty and contain only the new page to start writing to
		// on the next commit.
		LazyDeleteQueueT::QueueState s = self->m_lazyDeleteQueue.getState();
		ASSERT(s.numEntries == 0);
		ASSERT(s.numPages == 1);

		// The btree should now be a single non-oversized root page.
		ASSERT(self->m_header.height == 1);
		ASSERT(self->m_header.root.count == 1);

		// From the pager's perspective the only pages that should be in use are the btree root and
		// the previously mentioned lazy delete queue page.
		int64_t userPageCount = wait(self->m_pager->getUserPageCount());
		ASSERT(userPageCount == 2);

		return Void();
	}

	Future<Void> destroyAndCheckSanity() { return destroyAndCheckSanity_impl(this); }

private:
	struct ChildLinksRef {
		ChildLinksRef() = default;

		ChildLinksRef(VectorRef<RedwoodRecordRef> children, RedwoodRecordRef upperBound)
		  : children(children), upperBound(upperBound) {}

		ChildLinksRef(const RedwoodRecordRef* child, const RedwoodRecordRef* upperBound)
		  : children((RedwoodRecordRef*)child, 1), upperBound(*upperBound) {}

		ChildLinksRef(Arena& arena, const ChildLinksRef& toCopy)
		  : children(arena, toCopy.children), upperBound(arena, toCopy.upperBound) {}

		int expectedSize() const { return children.expectedSize() + upperBound.expectedSize(); }

		std::string toString() const {
			return format("{children=%s upperbound=%s}", ::toString(children).c_str(), upperBound.toString().c_str());
		}

		VectorRef<RedwoodRecordRef> children;
		RedwoodRecordRef upperBound;
	};

	// Utility class for building a vector of internal page entries.
	// Entries must be added in version order.  Modified will be set to true
	// if any entries differ from the original ones.  Additional entries will be
	// added when necessary to reconcile differences between the upper and lower
	// boundaries of consecutive entries.
	struct InternalPageBuilder {
		// Cursor must be at first entry in page
		InternalPageBuilder(const BTreePage::BinaryTree::Cursor& c) : cursor(c), modified(false), childPageCount(0) {}

	private:
		// This must be called internally, on records whose arena has already been added to the entries arena
		inline void addEntry(const RedwoodRecordRef& rec) {
			if (rec.value.present()) {
				++childPageCount;
			}

			// If no modification detected yet then check that this record is identical to the next
			// record from the original page which is at the current cursor position.
			if (!modified) {
				if (cursor.valid()) {
					if (rec != cursor.get()) {
						debug_printf("InternalPageBuilder: Found internal page difference.  new: %s  old: %s\n",
						             rec.toString().c_str(), cursor.get().toString().c_str());
						modified = true;
					} else {
						cursor.moveNext();
					}
				} else {
					debug_printf("InternalPageBuilder: Found internal page difference.  new: %s  old: <end>\n",
					             rec.toString().c_str());
					modified = true;
				}
			}

			entries.push_back(entries.arena(), rec);
		}

	public:
		// Add the child entries from newSet into entries
		void addEntries(ChildLinksRef newSet) {
			// If there are already entries, the last one links to a child page, and its upper bound is not the same
			// as the first lowerBound in newSet (or newSet is empty, as the next newSet is necessarily greater)
			// then add the upper bound of the previous set as a value-less record so that on future reads
			// the previous child page can be decoded correctly.
			if (!entries.empty() && entries.back().value.present() &&
			    (newSet.children.empty() || !newSet.children.front().sameExceptValue(lastUpperBound))) {
				debug_printf("InternalPageBuilder: Added placeholder %s\n",
				             lastUpperBound.withoutValue().toString().c_str());
				addEntry(lastUpperBound.withoutValue());
			}

			for (auto& child : newSet.children) {
				debug_printf("InternalPageBuilder: Adding child entry %s\n", child.toString().c_str());
				addEntry(child);
			}

			lastUpperBound = newSet.upperBound;
			debug_printf("InternalPageBuilder: New upper bound: %s\n", lastUpperBound.toString().c_str());
		}

		// Finish comparison to existing data if necesary.
		// Handle possible page upper bound changes.
		// If modified is set (see below) and our rightmost entry has a child page and its upper bound
		// (currently in lastUpperBound) does not match the new desired page upper bound, passed as newUpperBound,
		// then write lastUpperBound with no value to allow correct decoding of the rightmost entry.
		// This is only done if modified is set to avoid rewriting this page for this purpose only.
		//
		// After this call, lastUpperBound is internal page's upper bound.
		void finalize(const RedwoodRecordRef& upperBound, const RedwoodRecordRef& decodeUpperBound) {
			debug_printf(
			    "InternalPageBuilder::end  modified=%d  upperBound=%s  decodeUpperBound=%s  lastUpperBound=%s\n",
			    modified, upperBound.toString().c_str(), decodeUpperBound.toString().c_str(),
			    lastUpperBound.toString().c_str());
			modified = modified || cursor.valid();
			debug_printf("InternalPageBuilder::end  modified=%d after cursor check\n", modified);

			// If there are boundary key entries and the last one has a child page then the
			// upper bound for this internal page must match the required upper bound for
			// the last child entry.
			if (!entries.empty() && entries.back().value.present()) {
				debug_printf("InternalPageBuilder::end  last entry is not null\n");

				// If the page contents were not modified so far and the upper bound required
				// for the last child page (lastUpperBound) does not match what the page
				// was encoded with then the page must be modified.
				if (!modified && !lastUpperBound.sameExceptValue(decodeUpperBound)) {
					debug_printf("InternalPageBuilder::end  modified set true because lastUpperBound does not match "
					             "decodeUpperBound\n");
					modified = true;
				}

				if (modified && !lastUpperBound.sameExceptValue(upperBound)) {
					debug_printf("InternalPageBuilder::end  Modified is true but lastUpperBound does not match "
					             "upperBound so adding placeholder\n");
					addEntry(lastUpperBound.withoutValue());
					lastUpperBound = upperBound;
				}
			}
			debug_printf(
			    "InternalPageBuilder::end  exit.  modified=%d  upperBound=%s  decodeUpperBound=%s  lastUpperBound=%s\n",
			    modified, upperBound.toString().c_str(), decodeUpperBound.toString().c_str(),
			    lastUpperBound.toString().c_str());
		}

		BTreePage::BinaryTree::Cursor cursor;
		Standalone<VectorRef<RedwoodRecordRef>> entries;
		RedwoodRecordRef lastUpperBound;
		bool modified;
		int childPageCount;
	};

	// Represents a change to a single key - set, clear, or atomic op
	struct SingleKeyMutation {
		// Clear
		SingleKeyMutation() : op(MutationRef::ClearRange) {}
		// Set
		SingleKeyMutation(Value val) : op(MutationRef::SetValue), value(val) {}
		// Atomic Op
		SingleKeyMutation(MutationRef::Type op, Value val) : op(op), value(val) {}

		MutationRef::Type op;
		Value value;

		inline bool isClear() const { return op == MutationRef::ClearRange; }
		inline bool isSet() const { return op == MutationRef::SetValue; }
		inline bool isAtomicOp() const { return !isSet() && !isClear(); }

		inline bool equalToSet(ValueRef val) { return isSet() && value == val; }

		inline RedwoodRecordRef toRecord(KeyRef userKey, Version version) const {
			// No point in serializing an atomic op, it needs to be coalesced to a real value.
			ASSERT(!isAtomicOp());

			if (isClear()) return RedwoodRecordRef(userKey, version);

			return RedwoodRecordRef(userKey, version, value);
		}

		std::string toString() const { return format("op=%d val='%s'", op, printable(value).c_str()); }
	};

	struct RangeMutation {
		RangeMutation() : boundaryChanged(false), clearAfterBoundary(false) {}

		bool boundaryChanged;
		Optional<ValueRef> boundaryValue; // Not present means cleared
		bool clearAfterBoundary;

		bool boundaryCleared() const { return boundaryChanged && !boundaryValue.present(); }

		// Returns true if this RangeMutation doesn't actually mutate anything
		bool noChanges() const { return !boundaryChanged && !clearAfterBoundary; }

		void clearBoundary() {
			boundaryChanged = true;
			boundaryValue.reset();
		}

		void clearAll() {
			clearBoundary();
			clearAfterBoundary = true;
		}

		void setBoundaryValue(ValueRef v) {
			boundaryChanged = true;
			boundaryValue = v;
		}

		bool boundarySet() const { return boundaryChanged && boundaryValue.present(); }

		std::string toString() const {
			return format("boundaryChanged=%d clearAfterBoundary=%d boundaryValue=%s", boundaryChanged,
			              clearAfterBoundary, ::toString(boundaryValue).c_str());
		}
	};

public:
#include "ArtMutationBuffer.h"
	struct MutationBufferStdMap {
		MutationBufferStdMap() {
			// Create range representing the entire keyspace.  This reduces edge cases to applying mutations
			// because now all existing keys are within some range in the mutation map.
			mutations[dbBegin.key];
			// Setting the dbEnd key to be cleared prevents having to treat a range clear to dbEnd as a special
			// case in order to avoid traversing down the rightmost edge of the tree.
			mutations[dbEnd.key].clearBoundary();
		}

	private:
		typedef std::map<KeyRef, RangeMutation> MutationsT;
		Arena arena;
		MutationsT mutations;

	public:
		struct iterator : public MutationsT::iterator {
			typedef MutationsT::iterator Base;
			iterator() = default;
			iterator(const MutationsT::iterator& i) : Base(i) {}

			const KeyRef& key() { return (*this)->first; }

			RangeMutation& mutation() { return (*this)->second; }
		};

		struct const_iterator : public MutationsT::const_iterator {
			typedef MutationsT::const_iterator Base;
			const_iterator() = default;
			const_iterator(const MutationsT::const_iterator& i) : Base(i) {}
			const_iterator(const MutationsT::iterator& i) : Base(i) {}

			const KeyRef& key() { return (*this)->first; }

			const RangeMutation& mutation() { return (*this)->second; }
		};

		// Return a T constructed in arena
		template <typename T>
		T copyToArena(const T& object) {
			return T(arena, object);
		}

		const_iterator upper_bound(const KeyRef& k) const { return mutations.upper_bound(k); }

		const_iterator lower_bound(const KeyRef& k) const { return mutations.lower_bound(k); }

		// erase [begin, end) from the mutation map
		void erase(const const_iterator& begin, const const_iterator& end) { mutations.erase(begin, end); }

		// Find or create a mutation buffer boundary for bound and return an iterator to it
		iterator insert(KeyRef boundary) {
			// Find the first split point in buffer that is >= key
			// Since the initial state of the mutation buffer contains the range '' through
			// the maximum possible key, our search had to have found something so we
			// can assume the iterator is valid.
			iterator ib = mutations.lower_bound(boundary);

			// If we found the boundary we are looking for, return its iterator
			if (ib.key() == boundary) {
				return ib;
			}

			// ib is our insert hint.  Copy boundary into arena and insert boundary into buffer
			boundary = KeyRef(arena, boundary);
			ib = mutations.insert(ib, { boundary, RangeMutation() });

			// ib is certainly > begin() because it is guaranteed that the empty string
			// boundary exists and the only way to have found that is to look explicitly
			// for it in which case we would have returned above.
			iterator iPrevious = ib;
			--iPrevious;
			// If the range we just divided was being cleared, then the dividing boundary key and range after it must
			// also be cleared
			if (iPrevious.mutation().clearAfterBoundary) {
				ib.mutation().clearAll();
			}

			return ib;
		}
	};
#define USE_ART_MUTATION_BUFFER 1

#ifdef USE_ART_MUTATION_BUFFER
	typedef struct MutationBufferART MutationBuffer;
#else
	typedef struct MutationBufferStdMap MutationBuffer;
#endif

private:
	/* Mutation Buffer Overview
	 *
	 * This structure's organization is meant to put pending updates for the btree in an order
	 * that makes it efficient to query all pending mutations across all pending versions which are
	 * relevant to a particular subtree of the btree.
	 *
	 * At the top level, it is a map of the start of a range being modified to a RangeMutation.
	 * The end of the range is map key (which is the next range start in the map).
	 *
	 * - The buffer starts out with keys '' and endKVV.key already populated.
	 *
	 * - When a new key is inserted into the buffer map, it is by definition
	 *   splitting an existing range so it should take on the rangeClearVersion of
	 *   the immediately preceding key which is the start of that range
	 *
	 * - Keys are inserted into the buffer map for every individual operation (set/clear/atomic)
	 *   key and for both the start and end of a range clear.
	 *
	 * - To apply a single clear, add it to the individual ops only if the last entry is not also a clear.
	 *
	 * - To apply a range clear, after inserting the new range boundaries do the following to the start
	 *   boundary and all successive boundaries < end
	 *      - set the range clear version if not already set
	 *      - add a clear to the startKeyMutations if the final entry is not a clear.
	 *
	 * - Note that there are actually TWO valid ways to represent
	 *       set c = val1 at version 1
	 *       clear c\x00 to z at version 2
	 *   with this model.  Either
	 *      c =     { rangeClearVersion = 2, startKeyMutations = { 1 => val1 }
	 *      z =     { rangeClearVersion = <not present>, startKeyMutations = {}
	 *   OR
	 *      c =     { rangeClearVersion = <not present>, startKeyMutations = { 1 => val1 }
	 *      c\x00 = { rangeClearVersion = 2, startKeyMutations = { 2 => <not present> }
	 *      z =     { rangeClearVersion = <not present>, startKeyMutations = {}
	 *
	 *   This is because the rangeClearVersion applies to a range begining with the first
	 *   key AFTER the start key, so that the logic for reading the start key is more simple
	 *   as it only involves consulting startKeyMutations.  When adding a clear range, the
	 *   boundary key insert/split described above is valid, and is what is currently done,
	 *   but it would also be valid to see if the last key before startKey is equal to
	 *   keyBefore(startKey), and if so that mutation buffer boundary key can be used instead
	 *   without adding an additional key to the buffer.

	 * TODO: A possible optimization here could be to only use existing btree leaf page boundaries as keys,
	 * with mutation point keys being stored in an unsorted strucutre under those boundary map keys,
	 * to be sorted later just before being merged into the existing leaf page.
	 */

	IPager2* m_pager;
	MutationBuffer* m_pBuffer;
	std::map<Version, MutationBuffer> m_mutationBuffers;

	Version m_writeVersion;
	Version m_lastCommittedVersion;
	Version m_newOldestVersion;
	Future<Void> m_latestCommit;
	Future<Void> m_init;
	std::string m_name;

	// MetaKey changes size so allocate space for it to expand into
	union {
		uint8_t headerSpace[sizeof(MetaKey) + sizeof(LogicalPageID) * 30];
		MetaKey m_header;
	};

	LazyDeleteQueueT m_lazyDeleteQueue;

	// Writes entries to 1 or more pages and return a vector of boundary keys with their IPage(s)
	ACTOR static Future<Standalone<VectorRef<RedwoodRecordRef>>> writePages(
	    VersionedBTree* self, const RedwoodRecordRef* lowerBound, const RedwoodRecordRef* upperBound,
	    VectorRef<RedwoodRecordRef> entries, int height, Version v, BTreePageID previousID) {
		ASSERT(entries.size() > 0);
		state Standalone<VectorRef<RedwoodRecordRef>> records;

		// This is how much space for the binary tree exists in the page, after the header
		state int blockSize = self->m_pager->getUsablePageSize();
		state int pageSize = blockSize - sizeof(BTreePage);
		state float fillFactor = 0.66; // TODO: Make this a knob
		state int pageFillTarget = pageSize * fillFactor;
		state int blockCount = 1;

		state int kvBytes = 0;
		state int compressedBytes = BTreePage::BinaryTree::emptyTreeSize();
		state bool largeTree = false;

		state int start = 0;
		state int i = 0;
		// The common prefix length between the first and last records are common to all records
		state int skipLen = entries.front().getCommonPrefixLen(entries.back());

		// Leaves can have just one record if it's large, but internal pages should have at least 4
		state int minimumEntries = (height == 1 ? 1 : 4);

		// Lower bound of the page being added to
		state RedwoodRecordRef pageLowerBound = lowerBound->withoutValue();
		state RedwoodRecordRef pageUpperBound;

		while (1) {
			// While there are still entries to add and the page isn't full enough, add an entry
			while (i < entries.size() && (i - start < minimumEntries || compressedBytes < pageFillTarget)) {
				const RedwoodRecordRef& entry = entries[i];

				// Get delta from previous record or page lower boundary if this is the first item in a page
				const RedwoodRecordRef& base = (i == start) ? pageLowerBound : entries[i - 1];

				// All record pairs in entries have skipLen bytes in common with each other, but for i == 0 the base is
				// lowerBound
				int skip = i == 0 ? 0 : skipLen;

				// In a delta tree, all common prefix bytes that can be borrowed, will be, but not necessarily
				// by the same records during the linear estimate of the built page size.  Since the key suffix bytes
				// and therefore the key prefix lengths can be distributed differently in the balanced tree, worst case
				// overhead for the delta size must be assumed.
				int deltaSize = entry.deltaSize(base, skip, true);

				int keySize = entry.key.size();
				int valueSize = entry.value.present() ? entry.value.get().size() : 0;

				int nodeSize = BTreePage::BinaryTree::Node::headerSize(largeTree) + deltaSize;
				debug_printf("Adding %3d of %3lu (i=%3d) klen %4d  vlen %5d  nodeSize %5d  deltaSize %5d  page usage: "
				             "%d/%d (%.2f%%)  record=%s\n",
				             i + 1, entries.size(), i, keySize, valueSize, nodeSize, deltaSize, compressedBytes,
				             pageSize, (float)compressedBytes / pageSize * 100, entry.toString(height == 1).c_str());

				// While the node doesn't fit, expand the page.
				// This is a loop because if the page size moves into "large" range for DeltaTree
				// then the overhead will increase, which could require another page expansion.
				int spaceAvailable = pageSize - compressedBytes;
				if (nodeSize > spaceAvailable) {
					// Figure out how many additional whole or partial blocks are needed
					// newBlocks = ceil ( additional space needed / block size)
					int newBlocks = 1 + (nodeSize - spaceAvailable - 1) / blockSize;
					int newPageSize = pageSize + (newBlocks * blockSize);

					// If we've moved into "large" page range for the delta tree then add additional overhead required
					if (!largeTree && newPageSize > BTreePage::BinaryTree::SmallSizeLimit) {
						largeTree = true;
						// Add increased overhead for the current node to nodeSize
						nodeSize += BTreePage::BinaryTree::LargeTreePerNodeExtraOverhead;
						// Add increased overhead for all previously added nodes
						compressedBytes += (i - start) * BTreePage::BinaryTree::LargeTreePerNodeExtraOverhead;

						// Update calculations above made with previous overhead sizes
						spaceAvailable = pageSize - compressedBytes;
						newBlocks = 1 + (nodeSize - spaceAvailable - 1) / blockSize;
						newPageSize = pageSize + (newBlocks * blockSize);
					}

					blockCount += newBlocks;
					pageSize = newPageSize;
					pageFillTarget = pageSize * fillFactor;
				}

				kvBytes += keySize + valueSize;
				compressedBytes += nodeSize;
				++i;
			}

			// Flush the accumulated records to a page
			state int nextStart = i;
			// If we are building internal pages and there is a record after this page (index nextStart) but it has an
			// empty childPage value then skip it. It only exists to serve as an upper boundary for a child page that
			// has not been rewritten in the current commit, and that purpose will now be served by the upper bound of
			// the page we are now building.
			if (height != 1 && nextStart < entries.size() && !entries[nextStart].value.present()) {
				++nextStart;
			}

			// Use the next entry as the upper bound, or upperBound if there are no more entries beyond this page
			pageUpperBound = (i == entries.size()) ? upperBound->withoutValue() : entries[i].withoutValue();

			// If this is a leaf page, and not the last one to be written, shorten the upper boundary
			state bool isLastPage = (nextStart == entries.size());
			if (!isLastPage && height == 1) {
				int commonPrefix = pageUpperBound.getCommonPrefixLen(entries[i - 1], 0);
				pageUpperBound.truncate(commonPrefix + 1);
			}

			state std::vector<Reference<IPage>> pages;
			BTreePage* btPage;

			if (blockCount == 1) {
				Reference<IPage> page = self->m_pager->newPageBuffer();
				btPage = (BTreePage*)page->mutate();
				pages.push_back(std::move(page));
			} else {
				ASSERT(blockCount > 1);
				int size = blockSize * blockCount;
				btPage = (BTreePage*)new uint8_t[size];
			}

			btPage->height = height;
			btPage->kvBytes = kvBytes;

			debug_printf(
			    "Building tree.  start=%d  i=%d  count=%d  page usage: %d/%d (%.2f%%) bytes\nlower: %s\nupper: %s\n",
			    start, i, i - start, compressedBytes, pageSize, (float)compressedBytes / pageSize * 100,
			    pageLowerBound.toString(false).c_str(), pageUpperBound.toString(false).c_str());

			int written =
			    btPage->tree().build(pageSize, &entries[start], &entries[i], &pageLowerBound, &pageUpperBound);
			if (written > pageSize) {
				debug_printf("ERROR:  Wrote %d bytes to %d byte page (%d blocks). recs %d  kvBytes %d  compressed %d\n",
				             written, pageSize, blockCount, i - start, kvBytes, compressedBytes);
				fprintf(stderr,
				        "ERROR:  Wrote %d bytes to %d byte page (%d blocks). recs %d  kvBytes %d  compressed %d\n",
				        written, pageSize, blockCount, i - start, kvBytes, compressedBytes);
				ASSERT(false);
			}

			// Create chunked pages
			// TODO: Avoid copying page bytes, but this is not trivial due to how pager checksums are currently handled.
			if (blockCount != 1) {
				// Mark the slack in the page buffer as defined
				VALGRIND_MAKE_MEM_DEFINED(((uint8_t*)btPage) + written, (blockCount * blockSize) - written);
				const uint8_t* rptr = (const uint8_t*)btPage;
				for (int b = 0; b < blockCount; ++b) {
					Reference<IPage> page = self->m_pager->newPageBuffer();
					memcpy(page->mutate(), rptr, blockSize);
					rptr += blockSize;
					pages.push_back(std::move(page));
				}
				delete[](uint8_t*) btPage;
			}

			// Write this btree page, which is made of 1 or more pager pages.
			state int p;
			state BTreePageID childPageID;

			// If we are only writing 1 page and it has the same BTreePageID size as the original then try to reuse the
			// LogicalPageIDs in previousID and try to update them atomically.
			bool isOnlyPage = isLastPage && (start == 0);
			if (isOnlyPage && previousID.size() == pages.size()) {
				for (p = 0; p < pages.size(); ++p) {
					LogicalPageID id = wait(self->m_pager->atomicUpdatePage(previousID[p], pages[p], v));
					childPageID.push_back(records.arena(), id);
				}
			} else {
				// Either the original page is being split, or it's not but it has changed BTreePageID size.
				// Either way, there is no point in reusing any of the original page IDs because the parent
				// must be rewritten anyway to count for the change in child count or child links.
				// Free the old IDs, but only once (before the first output record is added).
				if (records.empty()) {
					self->freeBtreePage(previousID, v);
				}
				for (p = 0; p < pages.size(); ++p) {
					LogicalPageID id = wait(self->m_pager->newPageID());
					self->m_pager->updatePage(id, pages[p]);
					childPageID.push_back(records.arena(), id);
				}
			}

			wait(yield());

			// Update activity counts
			++counts.pageWrites;
			if (pages.size() > 1) {
				counts.extPageWrites += pages.size() - 1;
			}

			debug_printf("Flushing %s  lastPage=%d  original=%s  start=%d  i=%d  count=%d  page usage: %d/%d (%.2f%%) "
			             "bytes\nlower: %s\nupper: %s\n",
			             toString(childPageID).c_str(), isLastPage, toString(previousID).c_str(), start, i, i - start,
			             compressedBytes, pageSize, (float)compressedBytes / pageSize * 100,
			             pageLowerBound.toString(false).c_str(), pageUpperBound.toString(false).c_str());

			if (REDWOOD_DEBUG) {
				for (int j = start; j < i; ++j) {
					debug_printf(" %3d: %s\n", j, entries[j].toString(height == 1).c_str());
				}
				ASSERT(pageLowerBound.key <= pageUpperBound.key);
			}

			// Push a new record onto the results set, without the child page, copying it into the records arena
			records.push_back_deep(records.arena(), pageLowerBound.withoutValue());
			// Set the child page value of the inserted record to childPageID, which has already been allocated in
			// records.arena() above
			records.back().setChildPage(childPageID);

			if (isLastPage) {
				break;
			}

			start = nextStart;
			kvBytes = 0;
			compressedBytes = BTreePage::BinaryTree::emptyTreeSize();
			pageLowerBound = pageUpperBound;
		}

		// If we're writing internal pages, if the last entry was the start of a new page and had an empty child link
		// then it would not be written to a page. This means that the upper boundary for the the page set being built
		// is not the upper bound of the final page in that set, so it must be added to the output set to preserve the
		// decodability of the subtree to its left. Fortunately, this is easy to detect because the loop above would
		// exit before i has reached the item count.
		if (height != 1 && i != entries.size()) {
			debug_printf("Adding dummy record to avoid writing useless page: %s\n",
			             pageUpperBound.toString(false).c_str());
			records.push_back_deep(records.arena(), pageUpperBound);
		}

		return records;
	}

	ACTOR static Future<Standalone<VectorRef<RedwoodRecordRef>>> buildNewRoot(
	    VersionedBTree* self, Version version, Standalone<VectorRef<RedwoodRecordRef>> records, int height) {
		debug_printf("buildNewRoot start version %" PRId64 ", %lu records\n", version, records.size());

		// While there are multiple child pages for this version we must write new tree levels.
		while (records.size() > 1) {
			self->m_header.height = ++height;
			Standalone<VectorRef<RedwoodRecordRef>> newRecords =
			    wait(writePages(self, &dbBegin, &dbEnd, records, height, version, BTreePageID()));
			debug_printf("Wrote a new root level at version %" PRId64 " height %d size %lu pages\n", version, height,
			             newRecords.size());
			records = newRecords;
		}

		return records;
	}

	class SuperPage : public IPage, ReferenceCounted<SuperPage>, public FastAllocated<SuperPage> {
	public:
		SuperPage(std::vector<Reference<const IPage>> pages) {
			int blockSize = pages.front()->size();
			m_size = blockSize * pages.size();
			m_data = new uint8_t[m_size];
			uint8_t* wptr = m_data;
			for (auto& p : pages) {
				ASSERT(p->size() == blockSize);
				memcpy(wptr, p->begin(), blockSize);
				wptr += blockSize;
			}
		}

		virtual ~SuperPage() { delete[] m_data; }

		virtual Reference<IPage> clone() const {
			return Reference<IPage>(new SuperPage({ Reference<const IPage>::addRef(this) }));
		}

		void addref() const { ReferenceCounted<SuperPage>::addref(); }

		void delref() const { ReferenceCounted<SuperPage>::delref(); }

		int size() const { return m_size; }

		uint8_t const* begin() const { return m_data; }

		uint8_t* mutate() { return m_data; }

	private:
		uint8_t* m_data;
		int m_size;
	};

	ACTOR static Future<Reference<const IPage>> readPage(Reference<IPagerSnapshot> snapshot, BTreePageID id,
	                                                     const RedwoodRecordRef* lowerBound,
	                                                     const RedwoodRecordRef* upperBound,
	                                                     bool forLazyDelete = false) {
		if (!forLazyDelete) {
			debug_printf("readPage() op=read %s @%" PRId64 " lower=%s upper=%s\n", toString(id).c_str(),
			             snapshot->getVersion(), lowerBound->toString().c_str(), upperBound->toString().c_str());
		} else {
			debug_printf("readPage() op=readForDeferredClear %s @%" PRId64 " \n", toString(id).c_str(),
			             snapshot->getVersion());
		}

		wait(yield());

		state Reference<const IPage> page;

		++counts.pageReads;
		if (id.size() == 1) {
			Reference<const IPage> p = wait(snapshot->getPhysicalPage(id.front(), !forLazyDelete, false));
			page = p;
		} else {
			ASSERT(!id.empty());
			counts.extPageReads += (id.size() - 1);
			std::vector<Future<Reference<const IPage>>> reads;
			for (auto& pageID : id) {
				reads.push_back(snapshot->getPhysicalPage(pageID, !forLazyDelete, false));
			}
			std::vector<Reference<const IPage>> pages = wait(getAll(reads));
			// TODO:  Cache reconstituted super pages somehow, perhaps with help from the Pager.
			page = Reference<const IPage>(new SuperPage(pages));
		}

		debug_printf("readPage() op=readComplete %s @%" PRId64 " \n", toString(id).c_str(), snapshot->getVersion());
		const BTreePage* pTreePage = (const BTreePage*)page->begin();

		if (!forLazyDelete && page->userData == nullptr) {
			debug_printf("readPage() Creating Reader for %s @%" PRId64 " lower=%s upper=%s\n", toString(id).c_str(),
			             snapshot->getVersion(), lowerBound->toString().c_str(), upperBound->toString().c_str());
			page->userData = new BTreePage::BinaryTree::Mirror(&pTreePage->tree(), lowerBound, upperBound);
			page->userDataDestructor = [](void* ptr) { delete (BTreePage::BinaryTree::Mirror*)ptr; };
		}

		if (!forLazyDelete) {
			debug_printf("readPage() %s\n",
			             pTreePage->toString(false, id, snapshot->getVersion(), lowerBound, upperBound).c_str());
		}

		return page;
	}

	static void preLoadPage(IPagerSnapshot* snapshot, BTreePageID id) {
		++counts.pagePreloads;
		counts.extPagePreloads += (id.size() - 1);

		for (auto pageID : id) {
			snapshot->getPhysicalPage(pageID, true, true);
		}
	}

	void freeBtreePage(BTreePageID btPageID, Version v) {
		// Free individual pages at v
		for (LogicalPageID id : btPageID) {
			m_pager->freePage(id, v);
		}
	}

	// Write new version of pageID at version v using page as its data.
	// Attempts to reuse original id(s) in btPageID, returns BTreePageID.
	ACTOR static Future<BTreePageID> updateBtreePage(VersionedBTree* self, BTreePageID oldID, Arena* arena,
	                                                 Reference<IPage> page, Version writeVersion) {
		state BTreePageID newID;
		newID.resize(*arena, oldID.size());

		if (oldID.size() == 1) {
			LogicalPageID id = wait(self->m_pager->atomicUpdatePage(oldID.front(), page, writeVersion));
			newID.front() = id;
		} else {
			state std::vector<Reference<IPage>> pages;
			const uint8_t* rptr = page->begin();
			int bytesLeft = page->size();
			while (bytesLeft > 0) {
				Reference<IPage> p = self->m_pager->newPageBuffer();
				int blockSize = p->size();
				memcpy(p->mutate(), rptr, blockSize);
				rptr += blockSize;
				bytesLeft -= blockSize;
				pages.push_back(p);
			}
			ASSERT(pages.size() == oldID.size());

			// Write pages, trying to reuse original page IDs
			state int i = 0;
			for (; i < pages.size(); ++i) {
				LogicalPageID id = wait(self->m_pager->atomicUpdatePage(oldID[i], pages[i], writeVersion));
				newID[i] = id;
			}
		}

		// Update activity counts
		++counts.pageWrites;
		if (newID.size() > 1) {
			counts.extPageWrites += newID.size() - 1;
		}

		return newID;
	}

	// Copy page and initialize a Mirror for reading it.
	Reference<IPage> cloneForUpdate(Reference<const IPage> page) {
		Reference<IPage> newPage = page->clone();

		auto oldMirror = (const BTreePage::BinaryTree::Mirror*)page->userData;
		auto newBTPage = (BTreePage*)newPage->mutate();

		newPage->userData =
		    new BTreePage::BinaryTree::Mirror(&newBTPage->tree(), oldMirror->lowerBound(), oldMirror->upperBound());
		newPage->userDataDestructor = [](void* ptr) { delete (BTreePage::BinaryTree::Mirror*)ptr; };
		return newPage;
	}

	// Returns list of (version, internal page records, required upper bound)
	// iMutationBoundary is greatest boundary <= lowerBound->key
	// iMutationBoundaryEnd is least boundary >= upperBound->key
	ACTOR static Future<Standalone<ChildLinksRef>> commitSubtree(
	    VersionedBTree* self, MutationBuffer* mutationBuffer,
	    // MutationBuffer::const_iterator iMutationBoundary, // = mutationBuffer->upper_bound(lowerBound->key);
	    // --iMutationBoundary; MutationBuffer::const_iterator iMutationBoundaryEnd, // =
	    // mutationBuffer->lower_bound(upperBound->key);
	    Reference<IPagerSnapshot> snapshot, BTreePageID rootID, bool isLeaf, const RedwoodRecordRef* lowerBound,
	    const RedwoodRecordRef* upperBound, const RedwoodRecordRef* decodeLowerBound,
	    const RedwoodRecordRef* decodeUpperBound, int skipLen = 0) {
		// skipLen = lowerBound->getCommonPrefixLen(*upperBound, skipLen);
		state std::string context;
		if (REDWOOD_DEBUG) {
			context = format("CommitSubtree(root=%s): ", toString(rootID).c_str());
		}

		state Version writeVersion = self->getLastCommittedVersion() + 1;
		state Standalone<ChildLinksRef> result;

		debug_printf("%s lower=%s upper=%s\n", context.c_str(), lowerBound->toString().c_str(),
		             upperBound->toString().c_str());
		debug_printf("%s decodeLower=%s decodeUpper=%s\n", context.c_str(), decodeLowerBound->toString().c_str(),
		             decodeUpperBound->toString().c_str());
		self->counts.commitToPageStart++;

		// Find the slice of the mutation buffer that is relevant to this subtree
		state MutationBuffer::const_iterator iMutationBoundary = mutationBuffer->upper_bound(lowerBound->key);
		--iMutationBoundary;
		state MutationBuffer::const_iterator iMutationBoundaryEnd = mutationBuffer->lower_bound(upperBound->key);

		if (REDWOOD_DEBUG) {
			debug_printf("%s ---------MUTATION BUFFER SLICE ---------------------\n", context.c_str());
			auto begin = iMutationBoundary;
			while (1) {
				debug_printf("%s Mutation: '%s':  %s\n", context.c_str(), printable(begin.key()).c_str(),
				             begin.mutation().toString().c_str());
				if (begin == iMutationBoundaryEnd) {
					break;
				}
				++begin;
			}
			debug_printf("%s -------------------------------------\n", context.c_str());
		}

		// iMutationBoundary is greatest boundary <= lowerBound->key
		// iMutationBoundaryEnd is least boundary >= upperBound->key

		// If one mutation range covers the entire subtree, then check if the entire subtree is modified,
		// unmodified, or possibly/partially modified.
		MutationBuffer::const_iterator iMutationBoundaryNext = iMutationBoundary;
		++iMutationBoundaryNext;
		if (iMutationBoundaryNext == iMutationBoundaryEnd) {
			// Cleared means the entire range covering the subtree was cleared.  It is assumed true
			// if the range starting after the lower mutation boundary was cleared, and then proven false
			// below if possible.
			bool cleared = iMutationBoundary.mutation().clearAfterBoundary;
			// Unchanged means the entire range covering the subtree was unchanged, it is assumed to be the
			// opposite of cleared() and then proven false below if possible.
			bool unchanged = !cleared;
			debug_printf("%s cleared=%d unchanged=%d\n", context.c_str(), cleared, unchanged);

			// If the lower mutation boundary key is the same as the subtree lower bound then whether or not
			// that key is being changed or cleared affects this subtree.
			if (iMutationBoundary.key() == lowerBound->key) {
				// If subtree will be cleared (so far) but the lower boundary key is not cleared then the subtree is not
				// cleared
				if (cleared && !iMutationBoundary.mutation().boundaryCleared()) {
					cleared = false;
					debug_printf("%s cleared=%d unchanged=%d\n", context.c_str(), cleared, unchanged);
				}
				// If the subtree looked unchanged (so far) but the lower boundary is is changed then the subtree is
				// changed
				if (unchanged && iMutationBoundary.mutation().boundaryChanged) {
					unchanged = false;
					debug_printf("%s cleared=%d unchanged=%d\n", context.c_str(), cleared, unchanged);
				}
			}

			// If the higher mutation boundary key is the same as the subtree upper bound key then whether
			// or not it is being changed or cleared affects this subtree.
			if ((cleared || unchanged) && iMutationBoundaryEnd.key() == upperBound->key) {
				// If the key is being changed then the records in this subtree with the same key must be removed
				// so the subtree is definitely not unchanged, though it may be cleared to achieve the same effect.
				if (iMutationBoundaryEnd.mutation().boundaryChanged) {
					unchanged = false;
					debug_printf("%s cleared=%d unchanged=%d\n", context.c_str(), cleared, unchanged);
				} else {
					// If the key is not being changed then the records in this subtree can't be removed so the
					// subtree is not being cleared.
					cleared = false;
					debug_printf("%s cleared=%d unchanged=%d\n", context.c_str(), cleared, unchanged);
				}
			}

			// The subtree cannot be both cleared and unchanged.
			ASSERT(!(cleared && unchanged));

			// If no changes in subtree
			if (unchanged) {
				result.contents() = ChildLinksRef(decodeLowerBound, decodeUpperBound);
				debug_printf("%s no changes on this subtree, returning %s\n", context.c_str(),
				             toString(result).c_str());
				return result;
			}

			// If subtree is cleared
			if (cleared) {
				debug_printf("%s %s cleared, deleting it, returning %s\n", context.c_str(), isLeaf ? "Page" : "Subtree",
				             toString(result).c_str());
				if (isLeaf) {
					self->freeBtreePage(rootID, writeVersion);
				} else {
					self->m_lazyDeleteQueue.pushBack(LazyDeleteQueueEntry{ writeVersion, rootID });
				}
				return result;
			}
		}

		self->counts.commitToPage++;
		state Reference<const IPage> page = wait(readPage(snapshot, rootID, decodeLowerBound, decodeUpperBound));
		state BTreePage* btPage = (BTreePage*)page->begin();
		ASSERT(isLeaf == btPage->isLeaf());
		debug_printf(
		    "%s commitSubtree(): %s\n", context.c_str(),
		    btPage->toString(false, rootID, snapshot->getVersion(), decodeLowerBound, decodeUpperBound).c_str());

		state BTreePage::BinaryTree::Cursor cursor;

		if (REDWOOD_DEBUG) {
			debug_printf("%s ---------MUTATION BUFFER SLICE ---------------------\n", context.c_str());
			auto begin = iMutationBoundary;
			while (1) {
				debug_printf("%s Mutation: '%s':  %s\n", context.c_str(), printable(begin.key()).c_str(),
				             begin.mutation().toString().c_str());
				if (begin == iMutationBoundaryEnd) {
					break;
				}
				++begin;
			}
			debug_printf("%s -------------------------------------\n", context.c_str());
		}

		// Leaf Page
		if (isLeaf) {
			// Try to update page unless it's an oversized page or empty or the boundaries have changed
			// TODO: Caller already knows if boundaries are the same.
			bool updating =
			    btPage->tree().numItems > 0 && !(*decodeLowerBound != *lowerBound || *decodeUpperBound != *upperBound);

			state Reference<IPage> newPage;
			// If replacement pages are written they will be at the minimum version seen in the mutations for this leaf
			bool changesMade = false;

			// If attempting an in-place page update, clone the page and read/modify the copy
			if (updating) {
				newPage = self->cloneForUpdate(page);
				cursor = getCursor(newPage);
			} else {
				// Otherwise read the old page
				cursor = getCursor(page);
			}

			// Couldn't make changes in place, so now do a linear merge and build new pages.
			state Standalone<VectorRef<RedwoodRecordRef>> merged;

			auto switchToLinearMerge = [&]() {
				updating = false;
				auto c = cursor;
				c.moveFirst();
				while (c != cursor) {
					debug_printf("%s catch-up adding %s\n", context.c_str(), c.get().toString().c_str());
					merged.push_back(merged.arena(), c.get());
					c.moveNext();
				}
			};

			// The first mutation buffer boundary has a key <= the first key in the page.

			cursor.moveFirst();
			debug_printf("%s Leaf page, applying changes.\n", context.c_str());

			// Now, process each mutation range and merge changes with existing data.
			bool firstMutationBoundary = true;
			while (iMutationBoundary != iMutationBoundaryEnd) {
				debug_printf("%s New mutation boundary: '%s': %s\n", context.c_str(),
				             printable(iMutationBoundary.key()).c_str(),
				             iMutationBoundary.mutation().toString().c_str());

				// Apply the change to the mutation buffer start boundary key only if
				//   - there actually is a change (whether a set or a clear, old records are to be removed)
				//   - either this is not the first boundary or it is but its key matches our lower bound key
				bool applyBoundaryChange = iMutationBoundary.mutation().boundaryChanged &&
				                           (!firstMutationBoundary || iMutationBoundary.key() >= lowerBound->key);
				firstMutationBoundary = false;

				// Iterate over records for the mutation boundary key, keep them unless the boundary key was changed or
				// we are not applying it
				while (cursor.valid() && cursor.get().key == iMutationBoundary.key()) {
					// If there were no changes to the key or we're not applying it
					if (!applyBoundaryChange) {
						// If not updating, add to the output set, otherwise skip ahead past the records for the
						// mutation boundary
						if (!updating) {
							merged.push_back(merged.arena(), cursor.get());
							debug_printf("%s Added %s [existing, boundary start]\n", context.c_str(),
							             cursor.get().toString().c_str());
						}
						cursor.moveNext();
					} else {
						changesMade = true;
						// If updating, erase from the page, otherwise do not add to the output set
						if (updating) {
							debug_printf("%s Erasing %s [existing, boundary start]\n", context.c_str(),
							             cursor.get().toString().c_str());
							cursor.erase();
						} else {
							debug_printf("%s Skipped %s [existing, boundary start]\n", context.c_str(),
							             cursor.get().toString().c_str());
							cursor.moveNext();
						}
					}
				}

				constexpr int maxHeightAllowed = 8;

				// Write the new record(s) for the mutation boundary start key if its value has been set
				// Clears of this key will have been processed above by not being erased from the updated page or
				// excluded from the merge output
				if (applyBoundaryChange && iMutationBoundary.mutation().boundarySet()) {
					RedwoodRecordRef rec(iMutationBoundary.key(), 0, iMutationBoundary.mutation().boundaryValue.get());
					changesMade = true;

					// If updating, add to the page, else add to the output set
					if (updating) {
						if (cursor.mirror->insert(rec, skipLen, maxHeightAllowed)) {
							debug_printf("%s Inserted %s [mutation, boundary start]\n", context.c_str(),
							             rec.toString().c_str());
						} else {
							debug_printf("%s Inserted failed for %s [mutation, boundary start]\n", context.c_str(),
							             rec.toString().c_str());
							switchToLinearMerge();
						}
					}

					if (!updating) {
						merged.push_back(merged.arena(), rec);
						debug_printf("%s Added %s [mutation, boundary start]\n", context.c_str(),
						             rec.toString().c_str());
					}
				}

				// Before advancing the iterator, get whether or not the records in the following range must be removed
				bool remove = iMutationBoundary.mutation().clearAfterBoundary;
				// Advance to the next boundary because we need to know the end key for the current range.
				++iMutationBoundary;
				if (iMutationBoundary == iMutationBoundaryEnd) {
					skipLen = 0;
				}

				debug_printf("%s Mutation range end: '%s'\n", context.c_str(),
				             printable(iMutationBoundary.key()).c_str());

				// Now handle the records up through but not including the next mutation boundary key
				RedwoodRecordRef end(iMutationBoundary.key());

				// If the records are being removed and we're not doing an in-place update
				// OR if we ARE doing an update but the records are NOT being removed, then just skip them.
				if (remove != updating) {
					// If not updating, then the records, if any exist, are being removed.  We don't know if there
					// actually are any but we must assume there are.
					if (!updating) {
						changesMade = true;
					}

					debug_printf("%s Seeking forward to next boundary (remove=%d updating=%d) %s\n", context.c_str(),
					             remove, updating, iMutationBoundary.key().toString().c_str());
					cursor.seekGreaterThanOrEqual(end, skipLen);
				} else {
					// Otherwise we must visit the records.  If updating, the visit is to erase them, and if doing a
					// linear merge than the visit is to add them to the output set.
					while (cursor.valid() && cursor.get().compare(end, skipLen) < 0) {
						if (updating) {
							debug_printf("%s Erasing %s [existing, boundary start]\n", context.c_str(),
							             cursor.get().toString().c_str());
							cursor.erase();
							changesMade = true;
						} else {
							merged.push_back(merged.arena(), cursor.get());
							debug_printf("%s Added %s [existing, middle]\n", context.c_str(),
							             merged.back().toString().c_str());
							cursor.moveNext();
						}
					}
				}
			}

			// If there are still more records, they have the same key as the end boundary
			if (cursor.valid()) {
				// If the end boundary is changing, we must remove the remaining records in this page
				bool remove = iMutationBoundaryEnd.mutation().boundaryChanged;
				if (remove) {
					changesMade = true;
				}

				// If we don't have to remove the records and we are updating, do nothing.
				// If we do have to remove the records and we are not updating, do nothing.
				if (remove != updating) {
					debug_printf("%s Ignoring remaining records, remove=%d updating=%d\n", context.c_str(), remove,
					             updating);
				} else {
					// If updating and the key is changing, we must visit the records to erase them.
					// If not updating and the key is not changing, we must visit the records to add them to the output
					// set.
					while (cursor.valid()) {
						if (updating) {
							debug_printf(
							    "%s Erasing %s and beyond [existing, matches changed upper mutation boundary]\n",
							    context.c_str(), cursor.get().toString().c_str());
							cursor.erase();
						} else {
							merged.push_back(merged.arena(), cursor.get());
							debug_printf("%s Added %s [existing, tail]\n", context.c_str(),
							             merged.back().toString().c_str());
							cursor.moveNext();
						}
					}
				}
			} else {
				debug_printf("%s No records matching mutation buffer end boundary key\n", context.c_str());
			}

			// No changes were actually made.  This could happen if the only mutations are clear ranges which do not
			// match any records.
			if (!changesMade) {
				result.contents() = ChildLinksRef(decodeLowerBound, decodeUpperBound);
				debug_printf("%s No changes were made during mutation merge, returning %s\n", context.c_str(),
				             toString(result).c_str());
				return result;
			} else {
				debug_printf("%s Changes were made, writing.\n", context.c_str());
			}

			writeVersion = self->getLastCommittedVersion() + 1;

			if (updating) {
				const BTreePage::BinaryTree& deltaTree = ((const BTreePage*)newPage->begin())->tree();
				if (deltaTree.numItems == 0) {
					debug_printf("%s Page updates cleared all entries, returning %s\n", context.c_str(),
					             toString(result).c_str());
					self->freeBtreePage(rootID, writeVersion);
					return result;
				} else {
					// Otherwise update it.
					BTreePageID newID =
					    wait(self->updateBtreePage(self, rootID, &result.arena(), newPage, writeVersion));

					// Set the child page ID, which has already been allocated in result.arena()
					RedwoodRecordRef* rec = new (result.arena()) RedwoodRecordRef(decodeLowerBound->withoutValue());
					rec->setChildPage(newID);

					result.contents() = ChildLinksRef(rec, decodeUpperBound);
					debug_printf("%s Page updated in-place, returning %s\n", context.c_str(), toString(result).c_str());
					++counts.pageUpdates;
					return result;
				}
			}

			// If everything in the page was deleted then this page should be deleted as of the new version
			// Note that if a single range clear covered the entire page then we should not get this far
			if (merged.empty()) {
				debug_printf("%s All leaf page contents were cleared, returning %s\n", context.c_str(),
				             toString(result).c_str());
				self->freeBtreePage(rootID, writeVersion);
				return result;
			}

			state Standalone<VectorRef<RedwoodRecordRef>> entries =
			    wait(writePages(self, lowerBound, upperBound, merged, btPage->height, writeVersion, rootID));
			result.arena().dependsOn(entries.arena());
			result.contents() = ChildLinksRef(entries, *upperBound);
			debug_printf("%s Merge complete, returning %s\n", context.c_str(), toString(result).c_str());
			return result;
		} else {
			// Internal Page
			ASSERT(!isLeaf);
			state std::vector<Future<Standalone<ChildLinksRef>>> futureChildren;

			cursor = getCursor(page);
			cursor.moveFirst();

			bool first = true;
			while (cursor.valid()) {
				// The lower bound for the first child is the lowerBound arg
				const RedwoodRecordRef& childLowerBound = first ? *lowerBound : cursor.get();
				first = false;

				// At this point we should never be at a null child page entry because the first entry of a page
				// can't be null and this loop will skip over null entries that come after non-null entries.
				ASSERT(cursor.get().value.present());

				// The decode lower bound is always the key of the child link record
				const RedwoodRecordRef& decodeChildLowerBound = cursor.get();

				BTreePageID pageID = cursor.get().getChildPage();
				ASSERT(!pageID.empty());

				// The decode upper bound is always the next key after the child link, or the decode upper bound for
				// this page
				const RedwoodRecordRef& decodeChildUpperBound = cursor.moveNext() ? cursor.get() : *decodeUpperBound;

				// But the decode upper bound might be a placeholder record with a null child link because
				// the subtree was previously deleted but the key needed to exist to enable decoding of the
				// previous child page which has not since been rewritten.
				if (cursor.valid() && !cursor.get().value.present()) {
					// There should only be one null child link entry, followed by a present link or the end of the page
					ASSERT(!cursor.moveNext() || cursor.get().value.present());
				}

				const RedwoodRecordRef& childUpperBound = cursor.valid() ? cursor.get() : *upperBound;

				debug_printf("%s recursing to %s lower=%s upper=%s decodeLower=%s decodeUpper=%s\n", context.c_str(),
				             toString(pageID).c_str(), childLowerBound.toString().c_str(),
				             childUpperBound.toString().c_str(), decodeChildLowerBound.toString().c_str(),
				             decodeChildUpperBound.toString().c_str());

				// If this page has height of 2 then its children are leaf nodes
				futureChildren.push_back(self->commitSubtree(self, mutationBuffer, snapshot, pageID,
				                                             btPage->height == 2, &childLowerBound, &childUpperBound,
				                                             &decodeChildLowerBound, &decodeChildUpperBound));
			}

			// Waiting one at a time makes debugging easier
			// TODO:  Is it better to use waitForAll()?
			state int k;
			for (k = 0; k < futureChildren.size(); ++k) {
				wait(success(futureChildren[k]));
			}

			if (REDWOOD_DEBUG) {
				debug_printf("%s Subtree update results\n", context.c_str());
				for (int i = 0; i < futureChildren.size(); ++i) {
					debug_printf("%s subtree result %s\n", context.c_str(), toString(futureChildren[i].get()).c_str());
				}
			}

			// All of the things added to pageBuilder will exist in the arenas inside futureChildren or will be
			// upperBound
			BTreePage::BinaryTree::Cursor c = getCursor(page);
			c.moveFirst();
			InternalPageBuilder pageBuilder(c);

			for (int i = 0; i < futureChildren.size(); ++i) {
				ChildLinksRef c = futureChildren[i].get();

				if (!c.children.empty()) {
					pageBuilder.addEntries(c);
				}
			}

			pageBuilder.finalize(*upperBound, *decodeUpperBound);

			// If page contents have changed
			if (pageBuilder.modified) {
				// If the page now has no children
				if (pageBuilder.childPageCount == 0) {
					debug_printf("%s All internal page children were deleted so deleting this page too, returning %s\n",
					             context.c_str(), toString(result).c_str());
					self->freeBtreePage(rootID, writeVersion);
					return result;
				} else {
					debug_printf("%s Internal page modified, creating replacements.\n", context.c_str());
					debug_printf("%s newChildren=%s  lastUpperBound=%s  upperBound=%s\n", context.c_str(),
					             toString(pageBuilder.entries).c_str(), pageBuilder.lastUpperBound.toString().c_str(),
					             upperBound->toString().c_str());
					debug_printf("pagebuilder entries: %s\n", ::toString(pageBuilder.entries).c_str());

					ASSERT(!pageBuilder.entries.back().value.present() ||
					       pageBuilder.lastUpperBound.sameExceptValue(*upperBound));

					Standalone<VectorRef<RedwoodRecordRef>> childEntries = wait(
					    holdWhile(pageBuilder.entries, writePages(self, lowerBound, upperBound, pageBuilder.entries,
					                                              btPage->height, writeVersion, rootID)));

					result.arena().dependsOn(childEntries.arena());
					result.contents() = ChildLinksRef(childEntries, *upperBound);
					debug_printf("%s Internal modified, returning %s\n", context.c_str(), toString(result).c_str());
					return result;
				}
			} else {
				result.contents() = ChildLinksRef(decodeLowerBound, decodeUpperBound);
				debug_printf("%s Page has no changes, returning %s\n", context.c_str(), toString(result).c_str());
				return result;
			}
		}
	}

	ACTOR static Future<Void> commit_impl(VersionedBTree* self) {
		state MutationBuffer* mutations = self->m_pBuffer;

		// No more mutations are allowed to be written to this mutation buffer we will commit
		// at m_writeVersion, which we must save locally because it could change during commit.
		self->m_pBuffer = nullptr;
		state Version writeVersion = self->m_writeVersion;

		// The latest mutation buffer start version is the one we will now (or eventually) commit.
		state Version mutationBufferStartVersion = self->m_mutationBuffers.rbegin()->first;

		// Replace the lastCommit future with a new one and then wait on the old one
		state Promise<Void> committed;
		Future<Void> previousCommit = self->m_latestCommit;
		self->m_latestCommit = committed.getFuture();

		// Wait for the latest commit to be finished.
		wait(previousCommit);

		self->m_pager->setOldestVersion(self->m_newOldestVersion);
		debug_printf("%s: Beginning commit of version %" PRId64 ", new oldest version set to %" PRId64 "\n",
		             self->m_name.c_str(), writeVersion, self->m_newOldestVersion);

		state bool lazyDeleteStop = false;
		state Future<int> lazyDelete = incrementalSubtreeClear(self, &lazyDeleteStop);

		// Get the latest version from the pager, which is what we will read at
		state Version latestVersion = self->m_pager->getLatestVersion();
		debug_printf("%s: pager latestVersion %" PRId64 "\n", self->m_name.c_str(), latestVersion);

		state Standalone<BTreePageID> rootPageID = self->m_header.root.get();
		state RedwoodRecordRef lowerBound = dbBegin.withPageID(rootPageID);
		Standalone<ChildLinksRef> newRootChildren =
		    wait(commitSubtree(self, mutations, self->m_pager->getReadSnapshot(latestVersion), rootPageID,
		                       self->m_header.height == 1, &lowerBound, &dbEnd, &lowerBound, &dbEnd));
		debug_printf("CommitSubtree(root %s) returned %s\n", toString(rootPageID).c_str(),
		             toString(newRootChildren).c_str());

		// If the old root was deleted, write a new empty tree root node and free the old roots
		if (newRootChildren.children.empty()) {
			debug_printf("Writing new empty root.\n");
			LogicalPageID newRootID = wait(self->m_pager->newPageID());
			Reference<IPage> page = self->m_pager->newPageBuffer();
			makeEmptyRoot(page);
			self->m_header.height = 1;
			self->m_pager->updatePage(newRootID, page);
			rootPageID = BTreePageID((LogicalPageID*)&newRootID, 1);
		} else {
			Standalone<VectorRef<RedwoodRecordRef>> newRootLevel(newRootChildren.children, newRootChildren.arena());
			if (newRootLevel.size() == 1) {
				rootPageID = newRootLevel.front().getChildPage();
			} else {
				// If the new root level's size is not 1 then build new root level(s)
				Standalone<VectorRef<RedwoodRecordRef>> newRootPage =
				    wait(buildNewRoot(self, latestVersion, newRootLevel, self->m_header.height));
				rootPageID = newRootPage.front().getChildPage();
			}
		}

		self->m_header.root.set(rootPageID, sizeof(headerSpace) - sizeof(m_header));

		lazyDeleteStop = true;
		wait(success(lazyDelete));
		debug_printf("Lazy delete freed %u pages\n", lazyDelete.get());

		self->m_pager->setCommitVersion(writeVersion);

		wait(self->m_lazyDeleteQueue.flush());
		self->m_header.lazyDeleteQueue = self->m_lazyDeleteQueue.getState();

		debug_printf("Setting metakey\n");
		self->m_pager->setMetaKey(self->m_header.asKeyRef());

		debug_printf("%s: Committing pager %" PRId64 "\n", self->m_name.c_str(), writeVersion);
		wait(self->m_pager->commit());
		debug_printf("%s: Committed version %" PRId64 "\n", self->m_name.c_str(), writeVersion);

		// Now that everything is committed we must delete the mutation buffer.
		// Our buffer's start version should be the oldest mutation buffer version in the map.
		ASSERT(mutationBufferStartVersion == self->m_mutationBuffers.begin()->first);
		self->m_mutationBuffers.erase(self->m_mutationBuffers.begin());

		self->m_lastCommittedVersion = writeVersion;
		++counts.commits;
		committed.send(Void());

		return Void();
	}

public:
	// InternalCursor is for seeking to and iterating over the leaf-level RedwoodRecordRef records in the tree.
	// The records could represent multiple values for the same key at different versions, including a non-present value
	// representing a clear. Currently, however, all records are at version 0 and no clears are present in the tree.
	struct InternalCursor {
	private:
		// Each InternalCursor's position is represented by a reference counted PageCursor, which links
		// to its parent PageCursor, up to a PageCursor representing a cursor on the root page.
		// PageCursors can be shared by many InternalCursors, making InternalCursor copying low overhead
		struct PageCursor : ReferenceCounted<PageCursor>, FastAllocated<PageCursor> {
			Reference<PageCursor> parent;
			BTreePageID pageID; // Only needed for debugging purposes
			Reference<const IPage> page;
			BTreePage::BinaryTree::Cursor cursor;

			// id will normally reference memory owned by the parent, which is okay because a reference to the parent
			// will be held in the cursor
			PageCursor(BTreePageID id, Reference<const IPage> page, Reference<PageCursor> parent = {})
			  : pageID(id), page(page), parent(parent), cursor(getCursor(page)) {}

			PageCursor(const PageCursor& toCopy)
			  : parent(toCopy.parent), pageID(toCopy.pageID), page(toCopy.page), cursor(toCopy.cursor) {}

			// Convenience method for copying a PageCursor
			Reference<PageCursor> copy() const { return Reference<PageCursor>(new PageCursor(*this)); }

			const BTreePage* btPage() const { return (const BTreePage*)page->begin(); }

			bool isLeaf() const { return btPage()->isLeaf(); }

			Future<Reference<PageCursor>> getChild(Reference<IPagerSnapshot> pager, int readAheadBytes = 0) {
				ASSERT(!isLeaf());
				BTreePage::BinaryTree::Cursor next = cursor;
				next.moveNext();
				const RedwoodRecordRef& rec = cursor.get();
				BTreePageID id = rec.getChildPage();
				Future<Reference<const IPage>> child = readPage(pager, id, &rec, &next.getOrUpperBound());

				// Read ahead siblings at level 2
				// TODO:  Application of readAheadBytes is not taking into account the size of the current page or any
				// of the adjacent pages it is preloading.
				if (readAheadBytes > 0 && btPage()->height == 2 && next.valid()) {
					do {
						debug_printf("preloading %s %d bytes left\n", ::toString(next.get().getChildPage()).c_str(),
						             readAheadBytes);
						// If any part of the page was already loaded then stop
						if (next.get().value.present()) {
							preLoadPage(pager.getPtr(), next.get().getChildPage());
							readAheadBytes -= page->size();
						}
					} while (readAheadBytes > 0 && next.moveNext());
				}

				return map(child, [=](Reference<const IPage> page) {
					return Reference<PageCursor>(new PageCursor(id, page, Reference<PageCursor>::addRef(this)));
				});
			}

			std::string toString() const {
				return format("%s, %s", ::toString(pageID).c_str(),
				              cursor.valid() ? cursor.get().toString().c_str() : "<invalid>");
			}
		};

		Standalone<BTreePageID> rootPageID;
		Reference<IPagerSnapshot> pager;
		Reference<PageCursor> pageCursor;

	public:
		InternalCursor() {}

		InternalCursor(Reference<IPagerSnapshot> pager, BTreePageID root) : pager(pager), rootPageID(root) {}

		std::string toString() const {
			std::string r;

			Reference<PageCursor> c = pageCursor;
			int maxDepth = 0;
			while (c) {
				c = c->parent;
				++maxDepth;
			}

			c = pageCursor;
			int depth = maxDepth;
			while (c) {
				r = format("[%d/%d: %s] ", depth--, maxDepth, c->toString().c_str()) + r;
				c = c->parent;
			}
			return r;
		}

		// Returns true if cursor position is a valid leaf page record
		bool valid() const { return pageCursor && pageCursor->isLeaf() && pageCursor->cursor.valid(); }

		// Returns true if cursor position is valid() and has a present record value
		bool present() const { return valid() && pageCursor->cursor.get().value.present(); }

		// Returns true if cursor position is present() and has an effective version <= v
		bool presentAtVersion(Version v) { return present() && pageCursor->cursor.get().version <= v; }

		// This is to enable an optimization for the case where all internal records are at the
		// same version and there are no implicit clears
		// *this MUST be valid()
		bool presentAtExactVersion(Version v) const { return present() && pageCursor->cursor.get().version == v; }

		// Returns true if cursor position is present() and has an effective version <= v
		bool validAtVersion(Version v) { return valid() && pageCursor->cursor.get().version <= v; }

		const RedwoodRecordRef& get() const { return pageCursor->cursor.get(); }

		// Ensure that pageCursor is not shared with other cursors so we can modify it
		void ensureUnshared() {
			if (!pageCursor->isSoleOwner()) {
				pageCursor = pageCursor->copy();
			}
		}

		Future<Void> moveToRoot() {
			// If pageCursor exists follow parent links to the root
			if (pageCursor) {
				while (pageCursor->parent) {
					pageCursor = pageCursor->parent;
				}
				return Void();
			}

			// Otherwise read the root page
			Future<Reference<const IPage>> root = readPage(pager, rootPageID, &dbBegin, &dbEnd);
			return map(root, [=](Reference<const IPage> p) {
				pageCursor = Reference<PageCursor>(new PageCursor(rootPageID, p));
				return Void();
			});
		}

		ACTOR Future<bool> seekLessThan_impl(InternalCursor* self, RedwoodRecordRef query, int prefetchBytes) {
			Future<Void> f = self->moveToRoot();
			// f will almost always be ready
			if (!f.isReady()) {
				wait(f);
			}

			self->ensureUnshared();
			loop {
				bool isLeaf = self->pageCursor->isLeaf();
				bool success = self->pageCursor->cursor.seekLessThan(query);

				// Skip backwards over internal page entries that do not link to child pages
				if (!isLeaf) {
					// While record has no value, move again
					while (success && !self->pageCursor->cursor.get().value.present()) {
						success = self->pageCursor->cursor.movePrev();
					}
				}

				if (success) {
					// If we found a record < query at a leaf page then return success
					if (isLeaf) {
						return true;
					}

					Reference<PageCursor> child = wait(self->pageCursor->getChild(self->pager, prefetchBytes));
					self->pageCursor = child;
				} else {
					// No records < query on this page, so move to immediate previous record at leaf level
					bool success = wait(self->move(false));
					return success;
				}
			}
		}

		Future<bool> seekLessThan(RedwoodRecordRef query, int prefetchBytes) {
			return seekLessThan_impl(this, query, prefetchBytes);
		}

		ACTOR Future<bool> move_impl(InternalCursor* self, bool forward) {
			// Try to move pageCursor, if it fails to go parent, repeat until it works or root cursor can't be moved
			while (1) {
				self->ensureUnshared();
				bool success = self->pageCursor->cursor.valid() &&
				               (forward ? self->pageCursor->cursor.moveNext() : self->pageCursor->cursor.movePrev());

				// Skip over internal page entries that do not link to child pages
				if (!self->pageCursor->isLeaf()) {
					// While record has no value, move again
					while (success && !self->pageCursor->cursor.get().value.present()) {
						success = forward ? self->pageCursor->cursor.moveNext() : self->pageCursor->cursor.movePrev();
					}
				}

				// Stop if successful or there's no parent to move to
				if (success || !self->pageCursor->parent) {
					break;
				}

				// Move to parent
				self->pageCursor = self->pageCursor->parent;
			}

			// If pageCursor not valid we've reached an end of the tree
			if (!self->pageCursor->cursor.valid()) {
				return false;
			}

			// While not on a leaf page, move down to get to one.
			while (!self->pageCursor->isLeaf()) {
				// Skip over internal page entries that do not link to child pages
				while (!self->pageCursor->cursor.get().value.present()) {
					bool success = forward ? self->pageCursor->cursor.moveNext() : self->pageCursor->cursor.movePrev();
					if (!success) {
						return false;
					}
				}

				Reference<PageCursor> child = wait(self->pageCursor->getChild(self->pager));
				forward ? child->cursor.moveFirst() : child->cursor.moveLast();
				self->pageCursor = child;
			}

			return true;
		}

		Future<bool> move(bool forward) { return move_impl(this, forward); }

		// Move to the first or last record of the database.
		ACTOR Future<bool> move_end(InternalCursor* self, bool begin) {
			Future<Void> f = self->moveToRoot();

			// f will almost always be ready
			if (!f.isReady()) {
				wait(f);
			}

			self->ensureUnshared();

			loop {
				// Move to first or last record in the page
				bool success = begin ? self->pageCursor->cursor.moveFirst() : self->pageCursor->cursor.moveLast();

				// Skip over internal page entries that do not link to child pages
				if (!self->pageCursor->isLeaf()) {
					// While record has no value, move past it
					while (success && !self->pageCursor->cursor.get().value.present()) {
						success = begin ? self->pageCursor->cursor.moveNext() : self->pageCursor->cursor.movePrev();
					}
				}

				// If it worked, return true if we've reached a leaf page otherwise go to the next child
				if (success) {
					if (self->pageCursor->isLeaf()) {
						return true;
					}

					Reference<PageCursor> child = wait(self->pageCursor->getChild(self->pager));
					self->pageCursor = child;
				} else {
					return false;
				}
			}
		}

		Future<bool> moveFirst() { return move_end(this, true); }
		Future<bool> moveLast() { return move_end(this, false); }
	};

	// Cursor is for reading and interating over user visible KV pairs at a specific version
	// KeyValueRefs returned become invalid once the cursor is moved
	class Cursor : public IStoreCursor, public ReferenceCounted<Cursor>, public FastAllocated<Cursor>, NonCopyable {
	public:
		Cursor(Reference<IPagerSnapshot> pageSource, BTreePageID root, Version internalRecordVersion)
		  : m_version(internalRecordVersion), m_cur1(pageSource, root), m_cur2(m_cur1) {}

		void addref() { ReferenceCounted<Cursor>::addref(); }
		void delref() { ReferenceCounted<Cursor>::delref(); }

	private:
		Version m_version;
		// If kv is valid
		//   - kv.key references memory held by cur1
		//   - If cur1 points to a non split KV pair
		//       - kv.value references memory held by cur1
		//       - cur2 points to the next internal record after cur1
		//     Else
		//       - kv.value references memory in arena
		//       - cur2 points to the first internal record of the split KV pair
		InternalCursor m_cur1;
		InternalCursor m_cur2;
		Arena m_arena;
		Optional<KeyValueRef> m_kv;

	public:
		Future<Void> findEqual(KeyRef key) override { return find_impl(this, key, 0); }
		Future<Void> findFirstEqualOrGreater(KeyRef key, int prefetchBytes) override {
			return find_impl(this, key, 1, prefetchBytes);
		}
		Future<Void> findLastLessOrEqual(KeyRef key, int prefetchBytes) override {
			return find_impl(this, key, -1, prefetchBytes);
		}

		Future<Void> next() override { return move(this, true); }
		Future<Void> prev() override { return move(this, false); }

		bool isValid() override { return m_kv.present(); }

		KeyRef getKey() override { return m_kv.get().key; }

		ValueRef getValue() override { return m_kv.get().value; }

		std::string toString(bool includePaths = false) const {
			std::string r;
			r += format("Cursor(%p) ver: %" PRId64 " ", this, m_version);
			if (m_kv.present()) {
				r += format("  KV: '%s' -> '%s'", m_kv.get().key.printable().c_str(),
				            m_kv.get().value.printable().c_str());
			} else {
				r += "  KV: <np>";
			}
			if (includePaths) {
				r += format("\n Cur1: %s", m_cur1.toString().c_str());
				r += format("\n Cur2: %s", m_cur2.toString().c_str());
			} else {
				if (m_cur1.valid()) {
					r += format("\n Cur1: %s", m_cur1.get().toString().c_str());
				}
				if (m_cur2.valid()) {
					r += format("\n Cur2: %s", m_cur2.get().toString().c_str());
				}
			}

			return r;
		}

	private:
		// find key in tree closest to or equal to key (at this cursor's version)
		// for less than or equal use cmp < 0
		// for greater than or equal use cmp > 0
		// for equal use cmp == 0
		ACTOR static Future<Void> find_impl(Cursor* self, KeyRef key, int cmp, int prefetchBytes = 0) {
			state RedwoodRecordRef query(key, self->m_version + 1);
			self->m_kv.reset();

			wait(success(self->m_cur1.seekLessThan(query, prefetchBytes)));
			debug_printf("find%sE(%s): %s\n", cmp > 0 ? "GT" : (cmp == 0 ? "" : "LT"), query.toString().c_str(),
			             self->toString().c_str());

			// If we found the target key with a present value then return it as it is valid for any cmp type
			if (self->m_cur1.present() && self->m_cur1.get().key == key) {
				debug_printf("Target key found.  Cursor: %s\n", self->toString().c_str());
				self->m_kv = self->m_cur1.get().toKeyValueRef();
				return Void();
			}

			// If cmp type is Equal and we reached here, we didn't find it
			if (cmp == 0) {
				return Void();
			}

			// cmp mode is GreaterThanOrEqual, so if we've reached here an equal key was not found and cur1 either
			// points to a lesser key or is invalid.
			if (cmp > 0) {
				// If cursor is invalid, query was less than the first key in database so go to the first record
				if (!self->m_cur1.valid()) {
					bool valid = wait(self->m_cur1.moveFirst());
					if (!valid) {
						self->m_kv.reset();
						return Void();
					}
				} else {
					// Otherwise, move forward until we find a key greater than the target key.
					// If multiversion data is present, the next record could have the same key as the initial
					// record found but be at a newer version.
					loop {
						bool valid = wait(self->m_cur1.move(true));
						if (!valid) {
							self->m_kv.reset();
							return Void();
						}

						if (self->m_cur1.get().key > key) {
							break;
						}
					}
				}

				// Get the next present key at the target version.  Handles invalid cursor too.
				wait(self->next());
			} else if (cmp < 0) {
				// cmp mode is LessThanOrEqual.  An equal key to the target key was already checked above, and the
				// search was for LessThan query, so cur1 is already in the right place.
				if (!self->m_cur1.valid()) {
					self->m_kv.reset();
					return Void();
				}

				// Move to previous present kv pair at the target version
				wait(self->prev());
			}

			return Void();
		}

		ACTOR static Future<Void> move(Cursor* self, bool fwd) {
			debug_printf("Cursor::move(%d): Start %s\n", fwd, self->toString().c_str());
			ASSERT(self->m_cur1.valid());

			// If kv is present then the key/version at cur1 was already returned so move to a new key
			// Move cur1 until failure or a new key is found, keeping prior record visited in cur2
			if (self->m_kv.present()) {
				ASSERT(self->m_cur1.valid());
				loop {
					self->m_cur2 = self->m_cur1;
					debug_printf("Cursor::move(%d): Advancing cur1 %s\n", fwd, self->toString().c_str());
					bool valid = wait(self->m_cur1.move(fwd));
					if (!valid || self->m_cur1.get().key != self->m_cur2.get().key) {
						break;
					}
				}
			}

			// Given two consecutive cursors c1 and c2, c1 represents a returnable record if
			//    c1 is present at exactly version v
			//  OR
			//    c1 is.presentAtVersion(v) && (!c2.validAtVersion() || c2.get().key != c1.get().key())
			// Note the distinction between 'present' and 'valid'.  Present means the value for the key
			// exists at the version (but could be the empty string) while valid just means the internal
			// record is in effect at that version but it could indicate that the key was cleared and
			// no longer exists from the user's perspective at that version
			if (self->m_cur1.valid()) {
				self->m_cur2 = self->m_cur1;
				debug_printf("Cursor::move(%d): Advancing cur2 %s\n", fwd, self->toString().c_str());
				wait(success(self->m_cur2.move(true)));
			}

			while (self->m_cur1.valid()) {

				if (self->m_cur1.get().version == self->m_version ||
				    (self->m_cur1.presentAtVersion(self->m_version) &&
				     (!self->m_cur2.validAtVersion(self->m_version) ||
				      self->m_cur2.get().key != self->m_cur1.get().key))) {
					self->m_kv = self->m_cur1.get().toKeyValueRef();
					return Void();
				}

				if (fwd) {
					// Moving forward, move cur2 forward and keep cur1 pointing to the prior (predecessor) record
					debug_printf("Cursor::move(%d): Moving forward %s\n", fwd, self->toString().c_str());
					self->m_cur1 = self->m_cur2;
					wait(success(self->m_cur2.move(true)));
				} else {
					// Moving backward, move cur1 backward and keep cur2 pointing to the prior (successor) record
					debug_printf("Cursor::move(%d): Moving backward %s\n", fwd, self->toString().c_str());
					self->m_cur2 = self->m_cur1;
					wait(success(self->m_cur1.move(false)));
				}
			}

			debug_printf("Cursor::move(%d): Exit, end of db reached.  Cursor = %s\n", fwd, self->toString().c_str());
			self->m_kv.reset();

			return Void();
		}
	};
};

#include "art_impl.h"

RedwoodRecordRef VersionedBTree::dbBegin(StringRef(), 0);
RedwoodRecordRef VersionedBTree::dbEnd(LiteralStringRef("\xff\xff\xff\xff\xff"));
VersionedBTree::Counts VersionedBTree::counts;

class KeyValueStoreRedwoodUnversioned : public IKeyValueStore {
public:
	KeyValueStoreRedwoodUnversioned(std::string filePrefix, UID logID) : m_filePrefix(filePrefix) {
		// TODO: This constructor should really just take an IVersionedStore
		IPager2* pager = new DWALPager(4096, filePrefix, 0);
		m_tree = new VersionedBTree(pager, filePrefix);
		m_init = catchError(init_impl(this));
	}

	Future<Void> init() { return m_init; }

	ACTOR Future<Void> init_impl(KeyValueStoreRedwoodUnversioned* self) {
		TraceEvent(SevInfo, "RedwoodInit").detail("FilePrefix", self->m_filePrefix);
		wait(self->m_tree->init());
		Version v = self->m_tree->getLatestVersion();
		self->m_tree->setWriteVersion(v + 1);
		TraceEvent(SevInfo, "RedwoodInitComplete").detail("FilePrefix", self->m_filePrefix);
		return Void();
	}

	ACTOR void shutdown(KeyValueStoreRedwoodUnversioned* self, bool dispose) {
		TraceEvent(SevInfo, "RedwoodShutdown").detail("FilePrefix", self->m_filePrefix).detail("Dispose", dispose);
		if (self->m_error.canBeSet()) {
			self->m_error.sendError(actor_cancelled()); // Ideally this should be shutdown_in_progress
		}
		self->m_init.cancel();
		Future<Void> closedFuture = self->m_tree->onClosed();
		if (dispose)
			self->m_tree->dispose();
		else
			self->m_tree->close();
		wait(closedFuture);
		self->m_closed.send(Void());
		TraceEvent(SevInfo, "RedwoodShutdownComplete")
		    .detail("FilePrefix", self->m_filePrefix)
		    .detail("Dispose", dispose);
		delete self;
	}

	void close() { shutdown(this, false); }

	void dispose() { shutdown(this, true); }

	Future<Void> onClosed() { return m_closed.getFuture(); }

	Future<Void> commit(bool sequential = false) {
		Future<Void> c = m_tree->commit();
		m_tree->setOldestVersion(m_tree->getLatestVersion());
		m_tree->setWriteVersion(m_tree->getWriteVersion() + 1);
		return catchError(c);
	}

	KeyValueStoreType getType() { return KeyValueStoreType::SSD_REDWOOD_V1; }

	StorageBytes getStorageBytes() { return m_tree->getStorageBytes(); }

	Future<Void> getError() { return delayed(m_error.getFuture()); };

	void clear(KeyRangeRef range, const Arena* arena = 0) {
		debug_printf("CLEAR %s\n", printable(range).c_str());
		m_tree->clear(range);
	}

	void set(KeyValueRef keyValue, const Arena* arena = NULL) {
		debug_printf("SET %s\n", printable(keyValue).c_str());
		m_tree->set(keyValue);
	}

	Future<Standalone<RangeResultRef>> readRange(KeyRangeRef keys, int rowLimit = 1 << 30, int byteLimit = 1 << 30) {
		debug_printf("READRANGE %s\n", printable(keys).c_str());
		return catchError(readRange_impl(this, keys, rowLimit, byteLimit));
	}

	ACTOR static Future<Standalone<RangeResultRef>> readRange_impl(KeyValueStoreRedwoodUnversioned* self, KeyRange keys,
	                                                               int rowLimit, int byteLimit) {
		self->m_tree->counts.getRanges++;
		state Standalone<RangeResultRef> result;
		state int accumulatedBytes = 0;
		ASSERT(byteLimit > 0);

		if (rowLimit == 0) {
			return result;
		}

		state Reference<IStoreCursor> cur = self->m_tree->readAtVersion(self->m_tree->getLastCommittedVersion());
		// Prefetch is currently only done in the forward direction
		state int prefetchBytes = rowLimit > 1 ? byteLimit : 0;

		if (rowLimit > 0) {
			wait(cur->findFirstEqualOrGreater(keys.begin, prefetchBytes));
			while (cur->isValid() && cur->getKey() < keys.end) {
				KeyValueRef kv(KeyRef(result.arena(), cur->getKey()), ValueRef(result.arena(), cur->getValue()));
				accumulatedBytes += kv.expectedSize();
				result.push_back(result.arena(), kv);
				if (--rowLimit == 0 || accumulatedBytes >= byteLimit) {
					break;
				}
				wait(cur->next());
			}
		} else {
			wait(cur->findLastLessOrEqual(keys.end));
			if (cur->isValid() && cur->getKey() == keys.end) wait(cur->prev());

			while (cur->isValid() && cur->getKey() >= keys.begin) {
				KeyValueRef kv(KeyRef(result.arena(), cur->getKey()), ValueRef(result.arena(), cur->getValue()));
				accumulatedBytes += kv.expectedSize();
				result.push_back(result.arena(), kv);
				if (++rowLimit == 0 || accumulatedBytes >= byteLimit) {
					break;
				}
				wait(cur->prev());
			}
		}

		result.more = rowLimit == 0 || accumulatedBytes >= byteLimit;
		if (result.more) {
			ASSERT(result.size() > 0);
			result.readThrough = result[result.size() - 1].key;
		}
		return result;
	}

	ACTOR static Future<Optional<Value>> readValue_impl(KeyValueStoreRedwoodUnversioned* self, Key key,
	                                                    Optional<UID> debugID) {
		self->m_tree->counts.gets++;
		state Reference<IStoreCursor> cur = self->m_tree->readAtVersion(self->m_tree->getLastCommittedVersion());

		wait(cur->findEqual(key));
		if (cur->isValid()) {
			return cur->getValue();
		}
		return Optional<Value>();
	}

	Future<Optional<Value>> readValue(KeyRef key, Optional<UID> debugID = Optional<UID>()) {
		return catchError(readValue_impl(this, key, debugID));
	}

	ACTOR static Future<Optional<Value>> readValuePrefix_impl(KeyValueStoreRedwoodUnversioned* self, Key key,
	                                                          int maxLength, Optional<UID> debugID) {
		self->m_tree->counts.gets++;
		state Reference<IStoreCursor> cur = self->m_tree->readAtVersion(self->m_tree->getLastCommittedVersion());

		wait(cur->findEqual(key));
		if (cur->isValid()) {
			Value v = cur->getValue();
			int len = std::min(v.size(), maxLength);
			return Value(cur->getValue().substr(0, len));
		}
		return Optional<Value>();
	}

	Future<Optional<Value>> readValuePrefix(KeyRef key, int maxLength, Optional<UID> debugID = Optional<UID>()) {
		return catchError(readValuePrefix_impl(this, key, maxLength, debugID));
	}

	virtual ~KeyValueStoreRedwoodUnversioned(){};

private:
	std::string m_filePrefix;
	VersionedBTree* m_tree;
	Future<Void> m_init;
	Promise<Void> m_closed;
	Promise<Void> m_error;

	template <typename T>
	inline Future<T> catchError(Future<T> f) {
		return forwardError(f, m_error);
	}
};

IKeyValueStore* keyValueStoreRedwoodV1(std::string const& filename, UID logID) {
	return new KeyValueStoreRedwoodUnversioned(filename, logID);
}

int randomSize(int max) {
	int n = pow(deterministicRandom()->random01(), 3) * max;
	return n;
}

StringRef randomString(Arena& arena, int len, char firstChar = 'a', char lastChar = 'z') {
	++lastChar;
	StringRef s = makeString(len, arena);
	for (int i = 0; i < len; ++i) {
		*(uint8_t*)(s.begin() + i) = (uint8_t)deterministicRandom()->randomInt(firstChar, lastChar);
	}
	return s;
}

Standalone<StringRef> randomString(int len, char firstChar = 'a', char lastChar = 'z') {
	Standalone<StringRef> s;
	(StringRef&)s = randomString(s.arena(), len, firstChar, lastChar);
	return s;
}

KeyValue randomKV(int maxKeySize = 10, int maxValueSize = 5) {
	int kLen = randomSize(1 + maxKeySize);
	int vLen = maxValueSize > 0 ? randomSize(maxValueSize) : 0;

	KeyValue kv;

	kv.key = randomString(kv.arena(), kLen, 'a', 'm');
	for (int i = 0; i < kLen; ++i) mutateString(kv.key)[i] = (uint8_t)deterministicRandom()->randomInt('a', 'm');

	if (vLen > 0) {
		kv.value = randomString(kv.arena(), vLen, 'n', 'z');
		for (int i = 0; i < vLen; ++i) mutateString(kv.value)[i] = (uint8_t)deterministicRandom()->randomInt('o', 'z');
	}

	return kv;
}

ACTOR Future<int> verifyRange(VersionedBTree* btree, Key start, Key end, Version v,
                              std::map<std::pair<std::string, Version>, Optional<std::string>>* written,
                              int* pErrorCount) {
	state int errors = 0;
	if (end <= start) end = keyAfter(start);

	state std::map<std::pair<std::string, Version>, Optional<std::string>>::const_iterator i =
	    written->lower_bound(std::make_pair(start.toString(), 0));
	state std::map<std::pair<std::string, Version>, Optional<std::string>>::const_iterator iEnd =
	    written->upper_bound(std::make_pair(end.toString(), 0));
	state std::map<std::pair<std::string, Version>, Optional<std::string>>::const_iterator iLast;

	state Reference<IStoreCursor> cur = btree->readAtVersion(v);
	debug_printf("VerifyRange(@%" PRId64 ", %s, %s): Start cur=%p\n", v, start.toHexString().c_str(),
	             end.toHexString().c_str(), cur.getPtr());

	// Randomly use the cursor for something else first.
	if (deterministicRandom()->coinflip()) {
		state Key randomKey = randomKV().key;
		debug_printf("VerifyRange(@%" PRId64 ", %s, %s): Dummy seek to '%s'\n", v, start.toHexString().c_str(),
		             end.toHexString().c_str(), randomKey.toString().c_str());
		wait(deterministicRandom()->coinflip() ? cur->findFirstEqualOrGreater(randomKey)
		                                       : cur->findLastLessOrEqual(randomKey));
	}

	debug_printf("VerifyRange(@%" PRId64 ", %s, %s): Actual seek\n", v, start.toHexString().c_str(),
	             end.toHexString().c_str());
	wait(cur->findFirstEqualOrGreater(start));

	state std::vector<KeyValue> results;

	while (cur->isValid() && cur->getKey() < end) {
		// Find the next written kv pair that would be present at this version
		while (1) {
			iLast = i;
			if (i == iEnd) break;
			++i;

			if (iLast->first.second <= v && iLast->second.present() &&
			    (i == iEnd || i->first.first != iLast->first.first || i->first.second > v)) {
				debug_printf("VerifyRange(@%" PRId64 ", %s, %s) Found key in written map: %s\n", v,
				             start.toHexString().c_str(), end.toHexString().c_str(), iLast->first.first.c_str());
				break;
			}
		}

		if (iLast == iEnd) {
			++errors;
			++*pErrorCount;
			printf("VerifyRange(@%" PRId64 ", %s, %s) ERROR: Tree key '%s' vs nothing in written map.\n", v,
			       start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str());
			break;
		}

		if (cur->getKey() != iLast->first.first) {
			++errors;
			++*pErrorCount;
			printf("VerifyRange(@%" PRId64 ", %s, %s) ERROR: Tree key '%s' vs written '%s'\n", v,
			       start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str(),
			       iLast->first.first.c_str());
			break;
		}
		if (cur->getValue() != iLast->second.get()) {
			++errors;
			++*pErrorCount;
			printf("VerifyRange(@%" PRId64 ", %s, %s) ERROR: Tree key '%s' has tree value '%s' vs written '%s'\n", v,
			       start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str(),
			       cur->getValue().toString().c_str(), iLast->second.get().c_str());
			break;
		}

		ASSERT(errors == 0);

		results.push_back(KeyValue(KeyValueRef(cur->getKey(), cur->getValue())));
		wait(cur->next());
	}

	// Make sure there are no further written kv pairs that would be present at this version.
	while (1) {
		iLast = i;
		if (i == iEnd) break;
		++i;
		if (iLast->first.second <= v && iLast->second.present() &&
		    (i == iEnd || i->first.first != iLast->first.first || i->first.second > v))
			break;
	}

	if (iLast != iEnd) {
		++errors;
		++*pErrorCount;
		printf("VerifyRange(@%" PRId64 ", %s, %s) ERROR: Tree range ended but written has @%" PRId64 " '%s'\n", v,
		       start.toHexString().c_str(), end.toHexString().c_str(), iLast->first.second, iLast->first.first.c_str());
	}

	debug_printf("VerifyRangeReverse(@%" PRId64 ", %s, %s): start\n", v, start.toHexString().c_str(),
	             end.toHexString().c_str());

	// Randomly use a new cursor at the same version for the reverse range read, if the version is still available for
	// opening new cursors
	if (v >= btree->getOldestVersion() && deterministicRandom()->coinflip()) {
		cur = btree->readAtVersion(v);
	}

	// Now read the range from the tree in reverse order and compare to the saved results
	wait(cur->findLastLessOrEqual(end));
	if (cur->isValid() && cur->getKey() == end) wait(cur->prev());

	state std::vector<KeyValue>::const_reverse_iterator r = results.rbegin();

	while (cur->isValid() && cur->getKey() >= start) {
		if (r == results.rend()) {
			++errors;
			++*pErrorCount;
			printf("VerifyRangeReverse(@%" PRId64 ", %s, %s) ERROR: Tree key '%s' vs nothing in written map.\n", v,
			       start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str());
			break;
		}

		if (cur->getKey() != r->key) {
			++errors;
			++*pErrorCount;
			printf("VerifyRangeReverse(@%" PRId64 ", %s, %s) ERROR: Tree key '%s' vs written '%s'\n", v,
			       start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str(),
			       r->key.toString().c_str());
			break;
		}
		if (cur->getValue() != r->value) {
			++errors;
			++*pErrorCount;
			printf("VerifyRangeReverse(@%" PRId64
			       ", %s, %s) ERROR: Tree key '%s' has tree value '%s' vs written '%s'\n",
			       v, start.toHexString().c_str(), end.toHexString().c_str(), cur->getKey().toString().c_str(),
			       cur->getValue().toString().c_str(), r->value.toString().c_str());
			break;
		}

		++r;
		wait(cur->prev());
	}

	if (r != results.rend()) {
		++errors;
		++*pErrorCount;
		printf("VerifyRangeReverse(@%" PRId64 ", %s, %s) ERROR: Tree range ended but written has '%s'\n", v,
		       start.toHexString().c_str(), end.toHexString().c_str(), r->key.toString().c_str());
	}

	return errors;
}

// Verify the result of point reads for every set or cleared key at the given version
ACTOR Future<int> seekAll(VersionedBTree* btree, Version v,
                          std::map<std::pair<std::string, Version>, Optional<std::string>>* written, int* pErrorCount) {
	state std::map<std::pair<std::string, Version>, Optional<std::string>>::const_iterator i = written->cbegin();
	state std::map<std::pair<std::string, Version>, Optional<std::string>>::const_iterator iEnd = written->cend();
	state int errors = 0;
	state Reference<IStoreCursor> cur = btree->readAtVersion(v);

	while (i != iEnd) {
		state std::string key = i->first.first;
		state Version ver = i->first.second;
		if (ver == v) {
			state Optional<std::string> val = i->second;
			debug_printf("Verifying @%" PRId64 " '%s'\n", ver, key.c_str());
			state Arena arena;
			wait(cur->findEqual(KeyRef(arena, key)));

			if (val.present()) {
				if (!(cur->isValid() && cur->getKey() == key && cur->getValue() == val.get())) {
					++errors;
					++*pErrorCount;
					if (!cur->isValid())
						printf("Verify ERROR: key_not_found: '%s' -> '%s' @%" PRId64 "\n", key.c_str(),
						       val.get().c_str(), ver);
					else if (cur->getKey() != key)
						printf("Verify ERROR: key_incorrect: found '%s' expected '%s' @%" PRId64 "\n",
						       cur->getKey().toString().c_str(), key.c_str(), ver);
					else if (cur->getValue() != val.get())
						printf("Verify ERROR: value_incorrect: for '%s' found '%s' expected '%s' @%" PRId64 "\n",
						       cur->getKey().toString().c_str(), cur->getValue().toString().c_str(), val.get().c_str(),
						       ver);
				}
			} else {
				if (cur->isValid() && cur->getKey() == key) {
					++errors;
					++*pErrorCount;
					printf("Verify ERROR: cleared_key_found: '%s' -> '%s' @%" PRId64 "\n", key.c_str(),
					       cur->getValue().toString().c_str(), ver);
				}
			}
		}
		++i;
	}
	return errors;
}

ACTOR Future<Void> verify(VersionedBTree* btree, FutureStream<Version> vStream,
                          std::map<std::pair<std::string, Version>, Optional<std::string>>* written, int* pErrorCount,
                          bool serial) {
	state Future<int> fRangeAll;
	state Future<int> fRangeRandom;
	state Future<int> fSeekAll;

	// Queue of committed versions still readable from btree
	state std::deque<Version> committedVersions;

	try {
		loop {
			state Version v = waitNext(vStream);
			committedVersions.push_back(v);

			// Remove expired versions
			while (!committedVersions.empty() && committedVersions.front() < btree->getOldestVersion()) {
				committedVersions.pop_front();
			}

			// Choose a random committed version, or sometimes the latest (which could be ahead of the latest version
			// from vStream)
			v = (committedVersions.empty() || deterministicRandom()->random01() < 0.25)
			        ? btree->getLastCommittedVersion()
			        : committedVersions[deterministicRandom()->randomInt(0, committedVersions.size())];
			debug_printf("Using committed version %" PRId64 "\n", v);
			// Get a cursor at v so that v doesn't get expired between the possibly serial steps below.
			state Reference<IStoreCursor> cur = btree->readAtVersion(v);

			debug_printf("Verifying entire key range at version %" PRId64 "\n", v);
			fRangeAll = verifyRange(btree, LiteralStringRef(""), LiteralStringRef("\xff\xff"), v, written, pErrorCount);
			if (serial) {
				wait(success(fRangeAll));
			}

			Key begin = randomKV().key;
			Key end = randomKV().key;
			debug_printf("Verifying range (%s, %s) at version %" PRId64 "\n", toString(begin).c_str(),
			             toString(end).c_str(), v);
			fRangeRandom = verifyRange(btree, begin, end, v, written, pErrorCount);
			if (serial) {
				wait(success(fRangeRandom));
			}

			debug_printf("Verifying seeks to each changed key at version %" PRId64 "\n", v);
			fSeekAll = seekAll(btree, v, written, pErrorCount);
			if (serial) {
				wait(success(fSeekAll));
			}

			wait(success(fRangeAll) && success(fRangeRandom) && success(fSeekAll));

			printf("Verified version %" PRId64 ", %d errors\n", v, *pErrorCount);

			if (*pErrorCount != 0) break;
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream && e.code() != error_code_transaction_too_old) {
			throw;
		}
	}
	return Void();
}

// Does a random range read, doesn't trap/report errors
ACTOR Future<Void> randomReader(VersionedBTree* btree) {
	try {
		state Reference<IStoreCursor> cur;
		loop {
			wait(yield());
			if (!cur || deterministicRandom()->random01() > .01) {
				Version v = btree->getLastCommittedVersion();
				cur = btree->readAtVersion(v);
			}

			state KeyValue kv = randomKV(10, 0);
			wait(cur->findFirstEqualOrGreater(kv.key));
			state int c = deterministicRandom()->randomInt(0, 100);
			while (cur->isValid() && c-- > 0) {
				wait(success(cur->next()));
				wait(yield());
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_transaction_too_old) {
			throw e;
		}
	}

	return Void();
}

struct IntIntPair {
	IntIntPair() {}
	IntIntPair(int k, int v) : k(k), v(v) {}

	IntIntPair(Arena& arena, const IntIntPair& toCopy) { *this = toCopy; }

	struct Delta {
		bool prefixSource;
		bool deleted;
		int dk;
		int dv;

		IntIntPair apply(const IntIntPair& base, Arena& arena) { return { base.k + dk, base.v + dv }; }

		void setPrefixSource(bool val) { prefixSource = val; }

		bool getPrefixSource() const { return prefixSource; }

		void setDeleted(bool val) { deleted = val; }

		bool getDeleted() const { return deleted; }

		int size() const { return sizeof(Delta); }

		std::string toString() const {
			return format("DELTA{prefixSource=%d deleted=%d dk=%d(0x%x) dv=%d(0x%x)}", prefixSource, deleted, dk, dk,
			              dv, dv);
		}
	};

	// For IntIntPair, skipLen will be in units of fields, not bytes
	int getCommonPrefixLen(const IntIntPair& other, int skip = 0) const {
		if (k == other.k) {
			if (v == other.v) {
				return 2;
			}
			return 1;
		}
		return 0;
	}

	int compare(const IntIntPair& rhs, int skip = 0) const {
		if (skip == 2) {
			return 0;
		}
		int cmp = (skip > 0) ? 0 : (k - rhs.k);

		if (cmp == 0) {
			cmp = v - rhs.v;
		}
		return cmp;
	}

	bool operator==(const IntIntPair& rhs) const { return compare(rhs) == 0; }

	bool operator<(const IntIntPair& rhs) const { return compare(rhs) < 0; }

	int deltaSize(const IntIntPair& base, int skipLen, bool worstcase) const { return sizeof(Delta); }

	int writeDelta(Delta& d, const IntIntPair& base, int commonPrefix = -1) const {
		d.prefixSource = false;
		d.deleted = false;
		d.dk = k - base.k;
		d.dv = v - base.v;
		return sizeof(Delta);
	}

	int k;
	int v;

	std::string toString() const { return format("{k=%d(0x%x) v=%d(0x%x)}", k, k, v, v); }
};

int deltaTest(RedwoodRecordRef rec, RedwoodRecordRef base) {
	std::vector<uint8_t> buf(rec.key.size() + rec.value.orDefault(StringRef()).size() + 20);
	RedwoodRecordRef::Delta& d = *(RedwoodRecordRef::Delta*)&buf.front();

	Arena mem;
	int expectedSize = rec.deltaSize(base, 0, false);
	int deltaSize = rec.writeDelta(d, base);
	RedwoodRecordRef decoded = d.apply(base, mem);

	if (decoded != rec || expectedSize != deltaSize || d.size() != deltaSize) {
		printf("\n");
		printf("Base:                %s\n", base.toString().c_str());
		printf("Record:              %s\n", rec.toString().c_str());
		printf("Decoded:             %s\n", decoded.toString().c_str());
		printf("deltaSize():         %d\n", expectedSize);
		printf("writeDelta():        %d\n", deltaSize);
		printf("d.size():            %d\n", d.size());
		printf("DeltaToString:       %s\n", d.toString().c_str());
		printf("RedwoodRecordRef::Delta test failure!\n");
		ASSERT(false);
	}

	return deltaSize;
}

RedwoodRecordRef randomRedwoodRecordRef(const std::string& keyBuffer, const std::string& valueBuffer) {
	RedwoodRecordRef rec;
	rec.key = StringRef((uint8_t*)keyBuffer.data(), deterministicRandom()->randomInt(0, keyBuffer.size()));
	if (deterministicRandom()->coinflip()) {
		rec.value = StringRef((uint8_t*)valueBuffer.data(), deterministicRandom()->randomInt(0, valueBuffer.size()));
	}

	int versionIntSize = deterministicRandom()->randomInt(0, 8) * 8;
	if (versionIntSize > 0) {
		--versionIntSize;
		int64_t max = ((int64_t)1 << versionIntSize) - 1;
		rec.version = deterministicRandom()->randomInt64(0, max);
	}

	return rec;
}

TEST_CASE("!/redwood/correctness/unit/RedwoodRecordRef") {
	ASSERT(RedwoodRecordRef::Delta::LengthFormatSizes[0] == 3);
	ASSERT(RedwoodRecordRef::Delta::LengthFormatSizes[1] == 4);
	ASSERT(RedwoodRecordRef::Delta::LengthFormatSizes[2] == 6);
	ASSERT(RedwoodRecordRef::Delta::LengthFormatSizes[3] == 8);

	ASSERT(RedwoodRecordRef::Delta::VersionDeltaSizes[0] == 0);
	ASSERT(RedwoodRecordRef::Delta::VersionDeltaSizes[1] == 4);
	ASSERT(RedwoodRecordRef::Delta::VersionDeltaSizes[2] == 6);
	ASSERT(RedwoodRecordRef::Delta::VersionDeltaSizes[3] == 8);

	// Test pageID stuff.
	{
		LogicalPageID ids[] = { 1, 5 };
		BTreePageID id(ids, 2);
		RedwoodRecordRef r;
		r.setChildPage(id);
		ASSERT(r.getChildPage() == id);
		ASSERT(r.getChildPage().begin() == id.begin());

		Standalone<RedwoodRecordRef> r2 = r;
		ASSERT(r2.getChildPage() == id);
		ASSERT(r2.getChildPage().begin() != id.begin());
	}

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 0, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 0, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef("abc"), 0, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef("abc"), 0, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef("abc"), 0, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef("abcd"), 0, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef("abcd"), 2, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef("abc"), 2, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(std::string(300, 'k'), 2, std::string(1e6, 'v')),
	          RedwoodRecordRef(std::string(300, 'k'), 2, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 2, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 1, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 0xffff, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 1, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 1, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 0xffff, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 0xffffff, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 1, LiteralStringRef("")));

	deltaTest(RedwoodRecordRef(LiteralStringRef(""), 1, LiteralStringRef("")),
	          RedwoodRecordRef(LiteralStringRef(""), 0xffffff, LiteralStringRef("")));

	Arena mem;
	double start;
	uint64_t total;
	uint64_t count;
	uint64_t i;
	int64_t bytes;

	std::string keyBuffer(30000, 'k');
	std::string valueBuffer(70000, 'v');
	start = timer();
	count = 1000;
	bytes = 0;
	for (i = 0; i < count; ++i) {
		RedwoodRecordRef a = randomRedwoodRecordRef(keyBuffer, valueBuffer);
		RedwoodRecordRef b = randomRedwoodRecordRef(keyBuffer, valueBuffer);
		bytes += deltaTest(a, b);
	}
	double elapsed = timer() - start;
	printf("DeltaTest() on random large records %g M/s  %g MB/s\n", count / elapsed / 1e6, bytes / elapsed / 1e6);

	keyBuffer.resize(30);
	valueBuffer.resize(100);
	start = timer();
	count = 1e6;
	bytes = 0;
	for (i = 0; i < count; ++i) {
		RedwoodRecordRef a = randomRedwoodRecordRef(keyBuffer, valueBuffer);
		RedwoodRecordRef b = randomRedwoodRecordRef(keyBuffer, valueBuffer);
		bytes += deltaTest(a, b);
	}
	printf("DeltaTest() on random small records %g M/s  %g MB/s\n", count / elapsed / 1e6, bytes / elapsed / 1e6);

	RedwoodRecordRef rec1;
	RedwoodRecordRef rec2;

	rec1.key = LiteralStringRef("alksdfjaklsdfjlkasdjflkasdjfklajsdflk;ajsdflkajdsflkjadsf1");
	rec2.key = LiteralStringRef("alksdfjaklsdfjlkasdjflkasdjfklajsdflk;ajsdflkajdsflkjadsf234");

	rec1.version = deterministicRandom()->randomInt64(0, std::numeric_limits<Version>::max());
	rec2.version = deterministicRandom()->randomInt64(0, std::numeric_limits<Version>::max());

	start = timer();
	total = 0;
	count = 100e6;
	for (i = 0; i < count; ++i) {
		total += rec1.getCommonPrefixLen(rec2, 50);
	}
	printf("%" PRId64 " getCommonPrefixLen(skip=50) %g M/s\n", total, count / (timer() - start) / 1e6);

	start = timer();
	total = 0;
	count = 100e6;
	for (i = 0; i < count; ++i) {
		total += rec1.getCommonPrefixLen(rec2, 0);
	}
	printf("%" PRId64 " getCommonPrefixLen(skip=0) %g M/s\n", total, count / (timer() - start) / 1e6);

	char buf[1000];
	RedwoodRecordRef::Delta& d = *(RedwoodRecordRef::Delta*)buf;

	start = timer();
	total = 0;
	count = 100e6;
	int commonPrefix = rec1.getCommonPrefixLen(rec2, 0);

	for (i = 0; i < count; ++i) {
		total += rec1.writeDelta(d, rec2, commonPrefix);
	}
	printf("%" PRId64 " writeDelta(commonPrefix=%d) %g M/s\n", total, commonPrefix, count / (timer() - start) / 1e6);

	start = timer();
	total = 0;
	count = 10e6;
	for (i = 0; i < count; ++i) {
		total += rec1.writeDelta(d, rec2);
	}
	printf("%" PRId64 " writeDelta() %g M/s\n", total, count / (timer() - start) / 1e6);

	return Void();
}

TEST_CASE("!/redwood/correctness/unit/deltaTree/RedwoodRecordRef") {
	// Sanity check on delta tree node format
	ASSERT(DeltaTree<RedwoodRecordRef>::Node::headerSize(false) == 4);
	ASSERT(DeltaTree<RedwoodRecordRef>::Node::headerSize(true) == 8);

	const int N = deterministicRandom()->randomInt(200, 1000);

	RedwoodRecordRef prev;
	RedwoodRecordRef next(LiteralStringRef("\xff\xff\xff\xff"));

	Arena arena;
	std::set<RedwoodRecordRef> uniqueItems;

	// Add random items to uniqueItems until its size is N
	while (uniqueItems.size() < N) {
		std::string k = deterministicRandom()->randomAlphaNumeric(30);
		std::string v = deterministicRandom()->randomAlphaNumeric(30);
		RedwoodRecordRef rec;
		rec.key = StringRef(arena, k);
		rec.version = deterministicRandom()->coinflip()
		                  ? deterministicRandom()->randomInt64(0, std::numeric_limits<Version>::max())
		                  : invalidVersion;
		if (deterministicRandom()->coinflip()) {
			rec.value = StringRef(arena, v);
		}
		if (uniqueItems.count(rec) == 0) {
			uniqueItems.insert(rec);
		}
	}
	std::vector<RedwoodRecordRef> items(uniqueItems.begin(), uniqueItems.end());

	int bufferSize = N * 100;
	bool largeTree = bufferSize > DeltaTree<RedwoodRecordRef>::SmallSizeLimit;
	DeltaTree<RedwoodRecordRef>* tree = (DeltaTree<RedwoodRecordRef>*)new uint8_t[bufferSize];

	tree->build(bufferSize, &items[0], &items[items.size()], &prev, &next);

	printf("Count=%d  Size=%d  InitialHeight=%d  largeTree=%d\n", (int)items.size(), (int)tree->size(),
	       (int)tree->initialHeight, largeTree);
	debug_printf("Data(%p): %s\n", tree, StringRef((uint8_t*)tree, tree->size()).toHexString().c_str());

	DeltaTree<RedwoodRecordRef>::Mirror r(tree, &prev, &next);

	// Test delete/insert behavior for each item, making no net changes
	printf("Testing seek/delete/insert for existing keys with random values\n");
	ASSERT(tree->numItems == items.size());
	for (auto rec : items) {
		// Insert existing should fail
		ASSERT(!r.insert(rec));
		ASSERT(tree->numItems == items.size());

		// Erase existing should succeed
		ASSERT(r.erase(rec));
		ASSERT(tree->numItems == items.size() - 1);

		// Erase deleted should fail
		ASSERT(!r.erase(rec));
		ASSERT(tree->numItems == items.size() - 1);

		// Insert deleted should succeed
		ASSERT(r.insert(rec));
		ASSERT(tree->numItems == items.size());

		// Insert existing should fail
		ASSERT(!r.insert(rec));
		ASSERT(tree->numItems == items.size());
	}

	DeltaTree<RedwoodRecordRef>::Cursor fwd = r.getCursor();
	DeltaTree<RedwoodRecordRef>::Cursor rev = r.getCursor();

	DeltaTree<RedwoodRecordRef, RedwoodRecordRef::DeltaValueOnly>::Mirror rValuesOnly(tree, &prev, &next);
	DeltaTree<RedwoodRecordRef, RedwoodRecordRef::DeltaValueOnly>::Cursor fwdValueOnly = rValuesOnly.getCursor();

	printf("Verifying tree contents using forward, reverse, and value-only iterators\n");
	ASSERT(fwd.moveFirst());
	ASSERT(fwdValueOnly.moveFirst());
	ASSERT(rev.moveLast());

	int i = 0;
	while (1) {
		if (fwd.get() != items[i]) {
			printf("forward iterator i=%d\n  %s found\n  %s expected\n", i, fwd.get().toString().c_str(),
			       items[i].toString().c_str());
			printf("Delta: %s\n", fwd.node->raw->delta(largeTree).toString().c_str());
			ASSERT(false);
		}
		if (rev.get() != items[items.size() - 1 - i]) {
			printf("reverse iterator i=%d\n  %s found\n  %s expected\n", i, rev.get().toString().c_str(),
			       items[items.size() - 1 - i].toString().c_str());
			printf("Delta: %s\n", rev.node->raw->delta(largeTree).toString().c_str());
			ASSERT(false);
		}
		if (fwdValueOnly.get().value != items[i].value) {
			printf("forward values-only iterator i=%d\n  %s found\n  %s expected\n", i,
			       fwdValueOnly.get().toString().c_str(), items[i].toString().c_str());
			printf("Delta: %s\n", fwdValueOnly.node->raw->delta(largeTree).toString().c_str());
			ASSERT(false);
		}
		++i;

		bool more = fwd.moveNext();
		ASSERT(fwdValueOnly.moveNext() == more);
		ASSERT(rev.movePrev() == more);

		ASSERT(fwd.valid() == more);
		ASSERT(fwdValueOnly.valid() == more);
		ASSERT(rev.valid() == more);

		if (!fwd.valid()) {
			break;
		}
	}
	ASSERT(i == items.size());

	{
		DeltaTree<RedwoodRecordRef>::Mirror mirror(tree, &prev, &next);
		DeltaTree<RedwoodRecordRef>::Cursor c = mirror.getCursor();

		printf("Doing 20M random seeks using the same cursor from the same mirror.\n");
		double start = timer();

		for (int i = 0; i < 20000000; ++i) {
			const RedwoodRecordRef& query = items[deterministicRandom()->randomInt(0, items.size())];
			if (!c.seekLessThanOrEqual(query)) {
				printf("Not found!  query=%s\n", query.toString().c_str());
				ASSERT(false);
			}
			if (c.get() != query) {
				printf("Found incorrect node!  query=%s  found=%s\n", query.toString().c_str(),
				       c.get().toString().c_str());
				ASSERT(false);
			}
		}
		double elapsed = timer() - start;
		printf("Elapsed %f\n", elapsed);
	}

	{
		printf("Doing 5M random seeks using 10k random cursors, each from a different mirror.\n");
		double start = timer();
		std::vector<DeltaTree<RedwoodRecordRef>::Mirror*> mirrors;
		std::vector<DeltaTree<RedwoodRecordRef>::Cursor> cursors;
		for (int i = 0; i < 10000; ++i) {
			mirrors.push_back(new DeltaTree<RedwoodRecordRef>::Mirror(tree, &prev, &next));
			cursors.push_back(mirrors.back()->getCursor());
		}

		for (int i = 0; i < 5000000; ++i) {
			const RedwoodRecordRef& query = items[deterministicRandom()->randomInt(0, items.size())];
			DeltaTree<RedwoodRecordRef>::Cursor& c = cursors[deterministicRandom()->randomInt(0, cursors.size())];
			if (!c.seekLessThanOrEqual(query)) {
				printf("Not found!  query=%s\n", query.toString().c_str());
				ASSERT(false);
			}
			if (c.get() != query) {
				printf("Found incorrect node!  query=%s  found=%s\n", query.toString().c_str(),
				       c.get().toString().c_str());
				ASSERT(false);
			}
		}
		double elapsed = timer() - start;
		printf("Elapsed %f\n", elapsed);
	}

	return Void();
}

TEST_CASE("!/redwood/correctness/unit/deltaTree/IntIntPair") {
	const int N = 200;
	IntIntPair prev = { 1, 0 };
	IntIntPair next = { 10000, 10000 };

	state std::function<IntIntPair()> randomPair = [&]() {
		return IntIntPair(
		    { deterministicRandom()->randomInt(prev.k, next.k), deterministicRandom()->randomInt(prev.v, next.v) });
	};

	// Build a set of N unique items
	std::set<IntIntPair> uniqueItems;
	while (uniqueItems.size() < N) {
		IntIntPair p = randomPair();
		if (uniqueItems.count(p) == 0) {
			uniqueItems.insert(p);
		}
	}

	// Build tree of items
	std::vector<IntIntPair> items(uniqueItems.begin(), uniqueItems.end());
	int bufferSize = N * 2 * 20;
	DeltaTree<IntIntPair>* tree = (DeltaTree<IntIntPair>*)new uint8_t[bufferSize];
	int builtSize = tree->build(bufferSize, &items[0], &items[items.size()], &prev, &next);
	ASSERT(builtSize <= bufferSize);

	DeltaTree<IntIntPair>::Mirror r(tree, &prev, &next);

	// Grow uniqueItems until tree is full, adding half of new items to toDelete
	std::vector<IntIntPair> toDelete;
	while (1) {
		IntIntPair p = randomPair();
		if (uniqueItems.count(p) == 0) {
			if (!r.insert(p)) {
				break;
			};
			uniqueItems.insert(p);
			if (deterministicRandom()->coinflip()) {
				toDelete.push_back(p);
			}
			// printf("Inserted %s  size=%d\n", items.back().toString().c_str(), tree->size());
		}
	}

	ASSERT(tree->numItems > 2 * N);
	ASSERT(tree->size() <= bufferSize);

	// Update items vector
	items = std::vector<IntIntPair>(uniqueItems.begin(), uniqueItems.end());

	auto printItems = [&] {
		for (int k = 0; k < items.size(); ++k) {
			printf("%d %s\n", k, items[k].toString().c_str());
		}
	};

	printf("Count=%d  Size=%d  InitialHeight=%d  MaxHeight=%d\n", (int)items.size(), (int)tree->size(),
	       (int)tree->initialHeight, (int)tree->maxHeight);
	debug_printf("Data(%p): %s\n", tree, StringRef((uint8_t*)tree, tree->size()).toHexString().c_str());

	// Iterate through items and tree forward and backward, verifying tree contents.
	auto scanAndVerify = [&]() {
		printf("Verify tree contents.\n");

		DeltaTree<IntIntPair>::Cursor fwd = r.getCursor();
		DeltaTree<IntIntPair>::Cursor rev = r.getCursor();
		ASSERT(fwd.moveFirst());
		ASSERT(rev.moveLast());

		for (int i = 0; i < items.size(); ++i) {
			if (fwd.get() != items[i]) {
				printItems();
				printf("forward iterator i=%d\n  %s found\n  %s expected\n", i, fwd.get().toString().c_str(),
				       items[i].toString().c_str());
				ASSERT(false);
			}
			if (rev.get() != items[items.size() - 1 - i]) {
				printItems();
				printf("reverse iterator i=%d\n  %s found\n  %s expected\n", i, rev.get().toString().c_str(),
				       items[items.size() - 1 - i].toString().c_str());
				ASSERT(false);
			}

			// Advance iterator, check scanning cursors for correct validity state
			int j = i + 1;
			bool end = j == items.size();

			ASSERT(fwd.moveNext() == !end);
			ASSERT(rev.movePrev() == !end);
			ASSERT(fwd.valid() == !end);
			ASSERT(rev.valid() == !end);

			if (end) {
				break;
			}
		}
	};

	// Verify tree contents
	scanAndVerify();

	// Create a new mirror, decoding the tree from scratch since insert() modified both the tree and the mirror
	r = DeltaTree<IntIntPair>::Mirror(tree, &prev, &next);
	scanAndVerify();

	// For each randomly selected new item to be deleted, delete it from the DeltaTree and from uniqueItems
	printf("Deleting some items\n");
	for (auto p : toDelete) {
		uniqueItems.erase(p);
		DeltaTree<IntIntPair>::Cursor c = r.getCursor();
		ASSERT(c.seekLessThanOrEqual(p));
		c.erase();
	}
	// Update items vector
	items = std::vector<IntIntPair>(uniqueItems.begin(), uniqueItems.end());

	// Verify tree contents after deletions
	scanAndVerify();

	printf("Verifying insert/erase behavior for existing items\n");
	// Test delete/insert behavior for each item, making no net changes
	for (auto p : items) {
		// Insert existing should fail
		ASSERT(!r.insert(p));

		// Erase existing should succeed
		ASSERT(r.erase(p));

		// Erase deleted should fail
		ASSERT(!r.erase(p));

		// Insert deleted should succeed
		ASSERT(r.insert(p));

		// Insert existing should fail
		ASSERT(!r.insert(p));
	}

	// Tree contents should still match items vector
	scanAndVerify();

	printf("Verifying seek behaviors\n");
	DeltaTree<IntIntPair>::Cursor s = r.getCursor();

	// SeekLTE to each element
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		ASSERT(s.seekLessThanOrEqual(q));
		if (s.get() != p) {
			printItems();
			printf("seekLessThanOrEqual(%s) found %s expected %s\n", q.toString().c_str(), s.get().toString().c_str(),
			       p.toString().c_str());
			ASSERT(false);
		}
	}

	// SeekGTE to each element
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		ASSERT(s.seekGreaterThanOrEqual(q));
		if (s.get() != p) {
			printItems();
			printf("seekGreaterThanOrEqual(%s) found %s expected %s\n", q.toString().c_str(),
			       s.get().toString().c_str(), p.toString().c_str());
			ASSERT(false);
		}
	}

	// SeekLTE to the next possible int pair value after each element to make sure the base element is found
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		q.v++;
		ASSERT(s.seekLessThanOrEqual(q));
		if (s.get() != p) {
			printItems();
			printf("seekLessThanOrEqual(%s) found %s expected %s\n", q.toString().c_str(), s.get().toString().c_str(),
			       p.toString().c_str());
			ASSERT(false);
		}
	}

	// SeekGTE to the previous possible int pair value after each element to make sure the base element is found
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		q.v--;
		ASSERT(s.seekGreaterThanOrEqual(q));
		if (s.get() != p) {
			printItems();
			printf("seekGreaterThanOrEqual(%s) found %s expected %s\n", q.toString().c_str(),
			       s.get().toString().c_str(), p.toString().c_str());
			ASSERT(false);
		}
	}

	// SeekLTE to each element N times, using every element as a hint
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		for (int j = 0; j < items.size(); ++j) {
			ASSERT(s.seekLessThanOrEqual(items[j]));
			ASSERT(s.seekLessThanOrEqual(q, 0, &s));
			if (s.get() != p) {
				printItems();
				printf("i=%d  j=%d\n", i, j);
				printf("seekLessThanOrEqual(%s) found %s expected %s\n", q.toString().c_str(),
				       s.get().toString().c_str(), p.toString().c_str());
				ASSERT(false);
			}
		}
	}

	// SeekLTE to each element's next possible value, using each element as a hint
	for (int i = 0; i < items.size(); ++i) {
		IntIntPair p = items[i];
		IntIntPair q = p;
		q.v++;
		for (int j = 0; j < items.size(); ++j) {
			ASSERT(s.seekLessThanOrEqual(items[j]));
			ASSERT(s.seekLessThanOrEqual(q, 0, &s));
			if (s.get() != p) {
				printItems();
				printf("i=%d  j=%d\n", i, j);
				ASSERT(false);
			}
		}
	}

	auto skipSeekPerformance = [&](int jumpMax, bool old, bool useHint, int count) {
		// Skip to a series of increasing items, jump by up to jumpMax units forward in the
		// items, wrapping around to 0.
		double start = timer();
		s.moveFirst();
		auto first = s;
		int pos = 0;
		for (int c = 0; c < count; ++c) {
			int jump = deterministicRandom()->randomInt(0, jumpMax);
			int newPos = pos + jump;
			if (newPos >= items.size()) {
				pos = 0;
				newPos = jump;
				s = first;
			}
			IntIntPair q = items[newPos];
			++q.v;
			if (old) {
				if (useHint) {
					s.seekLessThanOrEqualOld(q, 0, &s, newPos - pos);
				} else {
					s.seekLessThanOrEqualOld(q, 0, nullptr, 0);
				}
			} else {
				if (useHint) {
					s.seekLessThanOrEqual(q, 0, &s, newPos - pos);
				} else {
					s.seekLessThanOrEqual(q);
				}
			}
			pos = newPos;
		}
		double elapsed = timer() - start;
		printf("Seek/skip test, jumpMax=%d, items=%d, oldSeek=%d useHint=%d:  Elapsed %f s\n", jumpMax, items.size(),
		       old, useHint, elapsed);
	};

	// Compare seeking to nearby elements with and without hints, using the old and new SeekLessThanOrEqual methods.
	// TODO:  Once seekLessThanOrEqual() with a hint is as fast as seekLessThanOrEqualOld, remove it.
	skipSeekPerformance(8, true, false, 80e6);
	skipSeekPerformance(8, true, true, 80e6);
	skipSeekPerformance(8, false, false, 80e6);
	skipSeekPerformance(8, false, true, 80e6);

	// Repeatedly seek for one of a set of pregenerated random pairs and time it.
	std::vector<IntIntPair> randomPairs;
	for (int i = 0; i < 10 * N; ++i) {
		randomPairs.push_back(randomPair());
	}

	// Random seeks
	double start = timer();
	for (int i = 0; i < 20000000; ++i) {
		IntIntPair p = randomPairs[i % randomPairs.size()];
		// Verify the result is less than or equal, and if seek fails then p must be lower than lowest (first) item
		if (!s.seekLessThanOrEqual(p)) {
			if (p >= items.front()) {
				printf("Seek failed!  query=%s  front=%s\n", p.toString().c_str(), items.front().toString().c_str());
				ASSERT(false);
			}
		} else if (s.get() > p) {
			printf("Found incorrect node!  query=%s  found=%s\n", p.toString().c_str(), s.get().toString().c_str());
			ASSERT(false);
		}
	}
	double elapsed = timer() - start;
	printf("Random seek test: Elapsed %f\n", elapsed);

	return Void();
}

struct SimpleCounter {
	SimpleCounter() : x(0), xt(0), t(timer()), start(t) {}
	void operator+=(int n) { x += n; }
	void operator++() { x++; }
	int64_t get() { return x; }
	double rate() {
		double t2 = timer();
		int r = (x - xt) / (t2 - t);
		xt = x;
		t = t2;
		return r;
	}
	double avgRate() { return x / (timer() - start); }
	int64_t x;
	double t;
	double start;
	int64_t xt;
	std::string toString() { return format("%" PRId64 "/%.2f/%.2f", x, rate() / 1e6, avgRate() / 1e6); }
};

TEST_CASE("!/redwood/performance/mutationBuffer") {
	// This test uses pregenerated short random keys
	int count = 10e6;

	printf("Generating %d strings...\n", count);
	Arena arena;
	std::vector<KeyRef> strings;
	while (strings.size() < count) {
		strings.push_back(randomString(arena, 5));
	}

	printf("Inserting and then finding each string...\n", count);
	double start = timer();
	VersionedBTree::MutationBuffer m;
	for (int i = 0; i < count; ++i) {
		KeyRef key = strings[i];
		auto a = m.insert(key);
		auto b = m.lower_bound(key);
		ASSERT(a == b);
		m.erase(a, b);
	}

	double elapsed = timer() - start;
	printf("count=%d elapsed=%f\n", count, elapsed);

	return Void();
}

TEST_CASE("!/redwood/correctness/btree") {
	state std::string pagerFile = "unittest_pageFile.redwood";
	IPager2* pager;

	state bool serialTest = deterministicRandom()->coinflip();
	state bool shortTest = deterministicRandom()->coinflip();

	state int pageSize =
	    shortTest ? 200 : (deterministicRandom()->coinflip() ? 4096 : deterministicRandom()->randomInt(200, 400));

	// We must be able to fit at least two any two keys plus overhead in a page to prevent
	// a situation where the tree cannot be grown upward with decreasing level size.
	state int maxKeySize = deterministicRandom()->randomInt(1, pageSize * 2);
	state int maxValueSize = randomSize(pageSize * 25);
	state int maxCommitSize = shortTest ? 1000 : randomSize(std::min<int>((maxKeySize + maxValueSize) * 20000, 10e6));
	state int mutationBytesTarget = shortTest ? 5000 : randomSize(std::min<int>(maxCommitSize * 100, 100e6));
	state double clearProbability = deterministicRandom()->random01() * .1;
	state double clearSingleKeyProbability = deterministicRandom()->random01();
	state double clearPostSetProbability = deterministicRandom()->random01() * .1;
	state double coldStartProbability = deterministicRandom()->random01();
	state double advanceOldVersionProbability = deterministicRandom()->random01();
	state double maxDuration = 60;

	printf("\n");
	printf("serialTest: %d\n", serialTest);
	printf("shortTest: %d\n", shortTest);
	printf("pageSize: %d\n", pageSize);
	printf("maxKeySize: %d\n", maxKeySize);
	printf("maxValueSize: %d\n", maxValueSize);
	printf("maxCommitSize: %d\n", maxCommitSize);
	printf("mutationBytesTarget: %d\n", mutationBytesTarget);
	printf("clearProbability: %f\n", clearProbability);
	printf("clearSingleKeyProbability: %f\n", clearSingleKeyProbability);
	printf("clearPostSetProbability: %f\n", clearPostSetProbability);
	printf("coldStartProbability: %f\n", coldStartProbability);
	printf("advanceOldVersionProbability: %f\n", advanceOldVersionProbability);
	printf("\n");

	printf("Deleting existing test data...\n");
	deleteFile(pagerFile);

	printf("Initializing...\n");
	state double startTime = now();
	pager = new DWALPager(pageSize, pagerFile, 0);
	state VersionedBTree* btree = new VersionedBTree(pager, pagerFile);
	wait(btree->init());

	state std::map<std::pair<std::string, Version>, Optional<std::string>> written;
	state std::set<Key> keys;

	state Version lastVer = btree->getLatestVersion();
	printf("Starting from version: %" PRId64 "\n", lastVer);

	state Version version = lastVer + 1;
	btree->setWriteVersion(version);

	state SimpleCounter mutationBytes;
	state SimpleCounter keyBytesInserted;
	state SimpleCounter valueBytesInserted;
	state SimpleCounter sets;
	state SimpleCounter rangeClears;
	state SimpleCounter keyBytesCleared;
	state int errorCount;
	state int mutationBytesThisCommit = 0;
	state int mutationBytesTargetThisCommit = randomSize(maxCommitSize);

	state PromiseStream<Version> committedVersions;
	state Future<Void> verifyTask = verify(btree, committedVersions.getFuture(), &written, &errorCount, serialTest);
	state Future<Void> randomTask = serialTest ? Void() : (randomReader(btree) || btree->getError());

	state Future<Void> commit = Void();

	while (mutationBytes.get() < mutationBytesTarget && (now() - startTime) < maxDuration) {
		if (now() - startTime > 600) {
			mutationBytesTarget = mutationBytes.get();
		}

		// Sometimes advance the version
		if (deterministicRandom()->random01() < 0.10) {
			++version;
			btree->setWriteVersion(version);
		}

		// Sometimes do a clear range
		if (deterministicRandom()->random01() < clearProbability) {
			Key start = randomKV(maxKeySize, 1).key;
			Key end = (deterministicRandom()->random01() < .01) ? keyAfter(start) : randomKV(maxKeySize, 1).key;

			// Sometimes replace start and/or end with a close actual (previously used) value
			if (deterministicRandom()->random01() < .10) {
				auto i = keys.upper_bound(start);
				if (i != keys.end()) start = *i;
			}
			if (deterministicRandom()->random01() < .10) {
				auto i = keys.upper_bound(end);
				if (i != keys.end()) end = *i;
			}

			// Do a single key clear based on probability or end being randomly chosen to be the same as begin
			// (unlikely)
			if (deterministicRandom()->random01() < clearSingleKeyProbability || end == start) {
				end = keyAfter(start);
			} else if (end < start) {
				std::swap(end, start);
			}

			// Apply clear range to verification map
			++rangeClears;
			KeyRangeRef range(start, end);
			debug_printf("      Mutation:  Clear '%s' to '%s' @%" PRId64 "\n", start.toString().c_str(),
			             end.toString().c_str(), version);
			auto e = written.lower_bound(std::make_pair(start.toString(), 0));
			if (e != written.end()) {
				auto last = e;
				auto eEnd = written.lower_bound(std::make_pair(end.toString(), 0));
				while (e != eEnd) {
					auto w = *e;
					++e;
					// If e key is different from last and last was present then insert clear for last's key at version
					if (last != eEnd &&
					    ((e == eEnd || e->first.first != last->first.first) && last->second.present())) {
						debug_printf("      Mutation:    Clearing key '%s' @%" PRId64 "\n", last->first.first.c_str(),
						             version);

						keyBytesCleared += last->first.first.size();
						mutationBytes += last->first.first.size();
						mutationBytesThisCommit += last->first.first.size();

						// If the last set was at version then just make it not present
						if (last->first.second == version) {
							last->second.reset();
						} else {
							written[std::make_pair(last->first.first, version)].reset();
						}
					}
					last = e;
				}
			}

			btree->clear(range);

			// Sometimes set the range start after the clear
			if (deterministicRandom()->random01() < clearPostSetProbability) {
				KeyValue kv = randomKV(0, maxValueSize);
				kv.key = range.begin;
				btree->set(kv);
				written[std::make_pair(kv.key.toString(), version)] = kv.value.toString();
			}
		} else {
			// Set a key
			KeyValue kv = randomKV(maxKeySize, maxValueSize);
			// Sometimes change key to a close previously used key
			if (deterministicRandom()->random01() < .01) {
				auto i = keys.upper_bound(kv.key);
				if (i != keys.end()) kv.key = StringRef(kv.arena(), *i);
			}

			debug_printf("      Mutation:  Set '%s' -> '%s' @%" PRId64 "\n", kv.key.toString().c_str(),
			             kv.value.toString().c_str(), version);

			++sets;
			keyBytesInserted += kv.key.size();
			valueBytesInserted += kv.value.size();
			mutationBytes += (kv.key.size() + kv.value.size());
			mutationBytesThisCommit += (kv.key.size() + kv.value.size());

			btree->set(kv);
			written[std::make_pair(kv.key.toString(), version)] = kv.value.toString();
			keys.insert(kv.key);
		}

		// Commit at end or after this commit's mutation bytes are reached
		if (mutationBytes.get() >= mutationBytesTarget || mutationBytesThisCommit >= mutationBytesTargetThisCommit) {
			// Wait for previous commit to finish
			wait(commit);
			printf("Committed.  Next commit %d bytes, %" PRId64
			       "/%d (%.2f%%)  Stats: Insert %.2f MB/s  ClearedKeys %.2f MB/s  Total %.2f\n",
			       mutationBytesThisCommit, mutationBytes.get(), mutationBytesTarget,
			       (double)mutationBytes.get() / mutationBytesTarget * 100,
			       (keyBytesInserted.rate() + valueBytesInserted.rate()) / 1e6, keyBytesCleared.rate() / 1e6,
			       mutationBytes.rate() / 1e6);

			Version v = version; // Avoid capture of version as a member of *this

			// Sometimes advance the oldest version to close the gap between the oldest and latest versions by a random
			// amount.
			if (deterministicRandom()->random01() < advanceOldVersionProbability) {
				btree->setOldestVersion(btree->getLastCommittedVersion() -
				                        deterministicRandom()->randomInt(0, btree->getLastCommittedVersion() -
				                                                                btree->getOldestVersion() + 1));
			}

			commit = map(btree->commit(), [=](Void) {
				printf("Committed: %s\n", VersionedBTree::counts.toString(true).c_str());
				// Notify the background verifier that version is committed and therefore readable
				committedVersions.send(v);
				return Void();
			});

			if (serialTest) {
				// Wait for commit, wait for verification, then start new verification
				wait(commit);
				committedVersions.sendError(end_of_stream());
				debug_printf("Waiting for verification to complete.\n");
				wait(verifyTask);
				committedVersions = PromiseStream<Version>();
				verifyTask = verify(btree, committedVersions.getFuture(), &written, &errorCount, serialTest);
			}

			mutationBytesThisCommit = 0;
			mutationBytesTargetThisCommit = randomSize(maxCommitSize);

			// Recover from disk at random
			if (!serialTest && deterministicRandom()->random01() < coldStartProbability) {
				printf("Recovering from disk after next commit.\n");

				// Wait for outstanding commit
				debug_printf("Waiting for outstanding commit\n");
				wait(commit);

				// Stop and wait for the verifier task
				committedVersions.sendError(end_of_stream());
				debug_printf("Waiting for verification to complete.\n");
				wait(verifyTask);

				debug_printf("Closing btree\n");
				Future<Void> closedFuture = btree->onClosed();
				btree->close();
				wait(closedFuture);

				printf("Reopening btree from disk.\n");
				IPager2* pager = new DWALPager(pageSize, pagerFile, 0);
				btree = new VersionedBTree(pager, pagerFile);
				wait(btree->init());

				Version v = btree->getLatestVersion();
				ASSERT(v == version);
				printf("Recovered from disk.  Latest version %" PRId64 "\n", v);

				// Create new promise stream and start the verifier again
				committedVersions = PromiseStream<Version>();
				verifyTask = verify(btree, committedVersions.getFuture(), &written, &errorCount, serialTest);
				randomTask = randomReader(btree) || btree->getError();
			}

			++version;
			btree->setWriteVersion(version);
		}

		// Check for errors
		if (errorCount != 0) throw internal_error();
	}

	debug_printf("Waiting for outstanding commit\n");
	wait(commit);
	committedVersions.sendError(end_of_stream());
	randomTask.cancel();
	debug_printf("Waiting for verification to complete.\n");
	wait(verifyTask);

	// Check for errors
	if (errorCount != 0) throw internal_error();

	wait(btree->destroyAndCheckSanity());

	Future<Void> closedFuture = btree->onClosed();
	btree->close();
	debug_printf("Closing.\n");
	wait(closedFuture);

	return Void();
}

ACTOR Future<Void> randomSeeks(VersionedBTree* btree, int count, char firstChar, char lastChar) {
	state Version readVer = btree->getLatestVersion();
	state int c = 0;
	state double readStart = timer();
	printf("Executing %d random seeks\n", count);
	state Reference<IStoreCursor> cur = btree->readAtVersion(readVer);
	while (c < count) {
		state Key k = randomString(20, firstChar, lastChar);
		wait(success(cur->findFirstEqualOrGreater(k)));
		++c;
	}
	double elapsed = timer() - readStart;
	printf("Random seek speed %d/s\n", int(count / elapsed));
	return Void();
}

ACTOR Future<Void> randomScans(VersionedBTree* btree, int count, int width, int readAhead, char firstChar,
                               char lastChar) {
	state Version readVer = btree->getLatestVersion();
	state int c = 0;
	state double readStart = timer();
	printf("Executing %d random scans\n", count);
	state Reference<IStoreCursor> cur = btree->readAtVersion(readVer);
	state bool adaptive = readAhead < 0;
	state int totalScanBytes = 0;
	while (c++ < count) {
		state Key k = randomString(20, firstChar, lastChar);
		wait(success(cur->findFirstEqualOrGreater(k, readAhead)));
		if (adaptive) {
			readAhead = totalScanBytes / c;
		}
		state int w = width;
		while (w > 0 && cur->isValid()) {
			totalScanBytes += cur->getKey().size();
			totalScanBytes += cur->getValue().size();
			wait(cur->next());
			--w;
		}
	}
	double elapsed = timer() - readStart;
	printf("Completed %d scans: readAhead=%d width=%d bytesRead=%d scansRate=%d/s\n", count, readAhead, width,
	       totalScanBytes, int(count / elapsed));
	return Void();
}

TEST_CASE("!/redwood/correctness/pager/cow") {
	state std::string pagerFile = "unittest_pageFile.redwood";
	printf("Deleting old test data\n");
	deleteFile(pagerFile);

	int pageSize = 4096;
	state IPager2* pager = new DWALPager(pageSize, pagerFile, 0);

	wait(success(pager->init()));
	state LogicalPageID id = wait(pager->newPageID());
	Reference<IPage> p = pager->newPageBuffer();
	memset(p->mutate(), (char)id, p->size());
	pager->updatePage(id, p);
	pager->setMetaKey(LiteralStringRef("asdfasdf"));
	wait(pager->commit());
	Reference<IPage> p2 = wait(pager->readPage(id, true));
	printf("%s\n", StringRef(p2->begin(), p2->size()).toHexString().c_str());

	// TODO: Verify reads, do more writes and reads to make this a real pager validator

	Future<Void> onClosed = pager->onClosed();
	pager->close();
	wait(onClosed);

	return Void();
}

TEST_CASE("!/redwood/performance/set") {
	state SignalableActorCollection actors;
	VersionedBTree::counts.clear();

	// If a test file is passed in by environment then don't write new data to it.
	state bool reload = getenv("TESTFILE") == nullptr;
	state std::string pagerFile = reload ? "unittest.redwood" : getenv("TESTFILE");

	if (reload) {
		printf("Deleting old test data\n");
		deleteFile(pagerFile);
	}

	state int pageSize = 4096;
	state int64_t pageCacheBytes = FLOW_KNOBS->PAGE_CACHE_4K;
	DWALPager* pager = new DWALPager(pageSize, pagerFile, pageCacheBytes);
	state VersionedBTree* btree = new VersionedBTree(pager, pagerFile);
	wait(btree->init());

	state int nodeCount = 1e9;
	state int maxChangesPerVersion = 5000;
	state int64_t kvBytesTarget = 4e9;
	state int commitTarget = 20e6;
	state int minKeyPrefixBytes = 25;
	state int maxKeyPrefixBytes = 25;
	state int minValueSize = 1000;
	state int maxValueSize = 2000;
	state int minConsecutiveRun = 1000;
	state int maxConsecutiveRun = 2000;
	state char firstKeyChar = 'a';
	state char lastKeyChar = 'm';

	printf("pageSize: %d\n", pageSize);
	printf("pageCacheBytes: %" PRId64 "\n", pageCacheBytes);
	printf("trailingIntegerIndexRange: %d\n", nodeCount);
	printf("maxChangesPerVersion: %d\n", maxChangesPerVersion);
	printf("minKeyPrefixBytes: %d\n", minKeyPrefixBytes);
	printf("maxKeyPrefixBytes: %d\n", maxKeyPrefixBytes);
	printf("minConsecutiveRun: %d\n", minConsecutiveRun);
	printf("maxConsecutiveRun: %d\n", maxConsecutiveRun);
	printf("minValueSize: %d\n", minValueSize);
	printf("maxValueSize: %d\n", maxValueSize);
	printf("commitTarget: %d\n", commitTarget);
	printf("kvBytesTarget: %" PRId64 "\n", kvBytesTarget);
	printf("KeyLexicon '%c' to '%c'\n", firstKeyChar, lastKeyChar);

	state int64_t kvBytes = 0;
	state int64_t kvBytesTotal = 0;
	state int records = 0;
	state Future<Void> commit = Void();
	state std::string value(maxValueSize, 'v');

	printf("Starting.\n");
	state double intervalStart = timer();
	state double start = intervalStart;

	if (reload) {
		while (kvBytesTotal < kvBytesTarget) {
			wait(yield());

			Version lastVer = btree->getLatestVersion();
			state Version version = lastVer + 1;
			btree->setWriteVersion(version);
			int changes = deterministicRandom()->randomInt(0, maxChangesPerVersion);

			while (changes > 0 && kvBytes < commitTarget) {
				KeyValue kv;
				kv.key = randomString(kv.arena(),
				                      deterministicRandom()->randomInt(minKeyPrefixBytes + sizeof(uint32_t),
				                                                       maxKeyPrefixBytes + sizeof(uint32_t) + 1),
				                      firstKeyChar, lastKeyChar);
				int32_t index = deterministicRandom()->randomInt(0, nodeCount);
				int runLength = deterministicRandom()->randomInt(minConsecutiveRun, maxConsecutiveRun + 1);

				while (runLength > 0 && changes > 0) {
					*(uint32_t*)(kv.key.end() - sizeof(uint32_t)) = bigEndian32(index++);
					kv.value = StringRef((uint8_t*)value.data(),
					                     deterministicRandom()->randomInt(minValueSize, maxValueSize + 1));

					btree->set(kv);

					--runLength;
					--changes;
					kvBytes += kv.key.size() + kv.value.size();
					++records;
				}
			}

			if (kvBytes >= commitTarget) {
				btree->setOldestVersion(btree->getLastCommittedVersion());
				wait(commit);
				printf("Cumulative %.2f MB keyValue bytes written at %.2f MB/s\n", kvBytesTotal / 1e6,
				       kvBytesTotal / (timer() - start) / 1e6);

				// Avoid capturing via this to freeze counter values
				int recs = records;
				int kvb = kvBytes;

				// Capturing invervalStart via this->intervalStart makes IDE's unhappy as they do not know about the
				// actor state object
				double* pIntervalStart = &intervalStart;

				commit = map(btree->commit(), [=](Void result) {
					printf("Committed: %s\n", VersionedBTree::counts.toString(true).c_str());
					double elapsed = timer() - *pIntervalStart;
					printf("Committed %d kvBytes in %d records in %f seconds, %.2f MB/s\n", kvb, recs, elapsed,
					       kvb / elapsed / 1e6);
					*pIntervalStart = timer();
					return Void();
				});

				kvBytesTotal += kvBytes;
				kvBytes = 0;
				records = 0;
			}
		}

		wait(commit);
		printf("Cumulative %.2f MB keyValue bytes written at %.2f MB/s\n", kvBytesTotal / 1e6,
		       kvBytesTotal / (timer() - start) / 1e6);
	}

	int seeks = 1e6;
	printf("Warming cache with seeks\n");
	actors.add(randomSeeks(btree, seeks / 3, firstKeyChar, lastKeyChar));
	actors.add(randomSeeks(btree, seeks / 3, firstKeyChar, lastKeyChar));
	actors.add(randomSeeks(btree, seeks / 3, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	state int ops = 10000;

	printf("Serial scans with adaptive readAhead...\n");
	actors.add(randomScans(btree, ops, 50, -1, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Serial scans with readAhead 3 pages...\n");
	actors.add(randomScans(btree, ops, 50, 12000, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Serial scans with readAhead 2 pages...\n");
	actors.add(randomScans(btree, ops, 50, 8000, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Serial scans with readAhead 1 page...\n");
	actors.add(randomScans(btree, ops, 50, 4000, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Serial scans...\n");
	actors.add(randomScans(btree, ops, 50, 0, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Serial seeks...\n");
	actors.add(randomSeeks(btree, ops, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	printf("Parallel seeks...\n");
	actors.add(randomSeeks(btree, ops, firstKeyChar, lastKeyChar));
	actors.add(randomSeeks(btree, ops, firstKeyChar, lastKeyChar));
	actors.add(randomSeeks(btree, ops, firstKeyChar, lastKeyChar));
	wait(actors.signalAndReset());
	printf("Stats: %s\n", VersionedBTree::counts.toString(true).c_str());

	Future<Void> closedFuture = btree->onClosed();
	btree->close();
	wait(closedFuture);

	return Void();
}

struct PrefixSegment {
	int length;
	int cardinality;

	std::string toString() const { return format("{%d bytes, %d choices}", length, cardinality); }
};

// Utility class for generating kv pairs under a prefix pattern
// It currently uses std::string in an abstraction breaking way.
struct KVSource {
	KVSource() {}

	typedef VectorRef<uint8_t> PrefixRef;
	typedef Standalone<PrefixRef> Prefix;

	std::vector<PrefixSegment> desc;
	std::vector<std::vector<std::string>> segments;
	std::vector<Prefix> prefixes;
	std::vector<Prefix*> prefixesSorted;
	std::string valueData;
	int prefixLen;
	int lastIndex;

	KVSource(const std::vector<PrefixSegment>& desc, int numPrefixes = 0) : desc(desc) {
		if (numPrefixes == 0) {
			numPrefixes = 1;
			for (auto& p : desc) {
				numPrefixes *= p.cardinality;
			}
		}

		prefixLen = 0;
		for (auto& s : desc) {
			prefixLen += s.length;
			std::vector<std::string> parts;
			while (parts.size() < s.cardinality) {
				parts.push_back(deterministicRandom()->randomAlphaNumeric(s.length));
			}
			segments.push_back(std::move(parts));
		}

		while (prefixes.size() < numPrefixes) {
			std::string p;
			for (auto& s : segments) {
				p.append(s[deterministicRandom()->randomInt(0, s.size())]);
			}
			prefixes.push_back(PrefixRef((uint8_t*)p.data(), p.size()));
		}

		for (auto& p : prefixes) {
			prefixesSorted.push_back(&p);
		}
		std::sort(prefixesSorted.begin(), prefixesSorted.end(), [](const Prefix* a, const Prefix* b) {
			return KeyRef((uint8_t*)a->begin(), a->size()) < KeyRef((uint8_t*)b->begin(), b->size());
		});

		valueData = deterministicRandom()->randomAlphaNumeric(100000);
		lastIndex = 0;
	}

	// Expands the chosen prefix in the prefix list to hold suffix,
	// fills suffix with random bytes, and returns a reference to the string
	KeyRef getKeyRef(int suffixLen) { return makeKey(randomPrefix(), suffixLen); }

	// Like getKeyRef but uses the same prefix as the last randomly chosen prefix
	KeyRef getAnotherKeyRef(int suffixLen, bool sorted = false) {
		Prefix& p = sorted ? *prefixesSorted[lastIndex] : prefixes[lastIndex];
		return makeKey(p, suffixLen);
	}

	// Like getKeyRef but gets a KeyRangeRef for two keys covering the given number of sorted adjacent prefixes
	KeyRangeRef getRangeRef(int prefixesCovered, int suffixLen) {
		prefixesCovered = std::min<int>(prefixesCovered, prefixes.size());
		int i = deterministicRandom()->randomInt(0, prefixesSorted.size() - prefixesCovered);
		Prefix* begin = prefixesSorted[i];
		Prefix* end = prefixesSorted[i + prefixesCovered];
		return KeyRangeRef(makeKey(*begin, suffixLen), makeKey(*end, suffixLen));
	}

	KeyRef getValue(int len) { return KeyRef(valueData).substr(0, len); }

	// Move lastIndex to the next position, wrapping around to 0
	void nextPrefix() {
		++lastIndex;
		if (lastIndex == prefixes.size()) {
			lastIndex = 0;
		}
	}

	Prefix& randomPrefix() {
		lastIndex = deterministicRandom()->randomInt(0, prefixes.size());
		return prefixes[lastIndex];
	}

	static KeyRef makeKey(Prefix& p, int suffixLen) {
		p.reserve(p.arena(), p.size() + suffixLen);
		uint8_t* wptr = p.end();
		for (int i = 0; i < suffixLen; ++i) {
			*wptr++ = (uint8_t)deterministicRandom()->randomAlphaNumeric();
		}
		return KeyRef(p.begin(), p.size() + suffixLen);
	}

	int numPrefixes() const { return prefixes.size(); };

	std::string toString() const {
		return format("{prefixLen=%d prefixes=%d format=%s}", prefixLen, numPrefixes(), ::toString(desc).c_str());
	}
};

std::string toString(const StorageBytes& sb) {
	return format("{%.2f MB total, %.2f MB free, %.2f MB available, %.2f MB used}", sb.total / 1e6, sb.free / 1e6,
	              sb.available / 1e6, sb.used / 1e6);
}

ACTOR Future<StorageBytes> getStableStorageBytes(IKeyValueStore* kvs) {
	state StorageBytes sb = kvs->getStorageBytes();

	// Wait for StorageBytes used metric to stabilize
	loop {
		wait(kvs->commit());
		StorageBytes sb2 = kvs->getStorageBytes();
		bool stable = sb2.used == sb.used;
		sb = sb2;
		if (stable) {
			break;
		}
	}

	return sb;
}

ACTOR Future<Void> prefixClusteredInsert(IKeyValueStore* kvs, int suffixSize, int valueSize, KVSource source,
                                         int recordCountTarget, bool usePrefixesInOrder) {
	state int commitTarget = 5e6;

	state int recordSize = source.prefixLen + suffixSize + valueSize;
	state int64_t kvBytesTarget = (int64_t)recordCountTarget * recordSize;
	state int recordsPerPrefix = recordCountTarget / source.numPrefixes();

	printf("\nstoreType: %d\n", kvs->getType());
	printf("commitTarget: %d\n", commitTarget);
	printf("prefixSource: %s\n", source.toString().c_str());
	printf("usePrefixesInOrder: %d\n", usePrefixesInOrder);
	printf("suffixSize: %d\n", suffixSize);
	printf("valueSize: %d\n", valueSize);
	printf("recordSize: %d\n", recordSize);
	printf("recordsPerPrefix: %d\n", recordsPerPrefix);
	printf("recordCountTarget: %d\n", recordCountTarget);
	printf("kvBytesTarget: %" PRId64 "\n", kvBytesTarget);

	state int64_t kvBytes = 0;
	state int64_t kvBytesTotal = 0;
	state int records = 0;
	state Future<Void> commit = Void();
	state std::string value = deterministicRandom()->randomAlphaNumeric(1e6);

	wait(kvs->init());

	state double intervalStart = timer();
	state double start = intervalStart;

	state std::function<void()> stats = [&]() {
		double elapsed = timer() - start;
		printf("Cumulative stats: %.2f seconds  %.2f MB keyValue bytes  %d records  %.2f MB/s  %.2f rec/s\r", elapsed,
		       kvBytesTotal / 1e6, records, kvBytesTotal / elapsed / 1e6, records / elapsed);
		fflush(stdout);
	};

	while (kvBytesTotal < kvBytesTarget) {
		wait(yield());

		state int i;
		for (i = 0; i < recordsPerPrefix; ++i) {
			KeyValueRef kv(source.getAnotherKeyRef(4, usePrefixesInOrder), source.getValue(valueSize));
			kvs->set(kv);
			kvBytes += kv.expectedSize();
			++records;

			if (kvBytes >= commitTarget) {
				wait(commit);
				stats();
				commit = kvs->commit();
				kvBytesTotal += kvBytes;
				if (kvBytesTotal >= kvBytesTarget) {
					break;
				}
				kvBytes = 0;
			}
		}

		// Use every prefix, one at a time
		source.nextPrefix();
	}

	wait(commit);
	stats();
	printf("\n");

	intervalStart = timer();
	StorageBytes sb = wait(getStableStorageBytes(kvs));
	printf("storageBytes: %s (stable after %.2f seconds)\n", toString(sb).c_str(), timer() - intervalStart);

	printf("Clearing all keys\n");
	intervalStart = timer();
	kvs->clear(KeyRangeRef(LiteralStringRef(""), LiteralStringRef("\xff")));
	state StorageBytes sbClear = wait(getStableStorageBytes(kvs));
	printf("Cleared all keys in %.2f seconds, final storageByte: %s\n", timer() - intervalStart,
	       toString(sbClear).c_str());

	return Void();
}

ACTOR Future<Void> sequentialInsert(IKeyValueStore* kvs, int prefixLen, int valueSize, int recordCountTarget) {
	state int commitTarget = 5e6;

	state KVSource source({ { prefixLen, 1 } });
	state int recordSize = source.prefixLen + sizeof(uint64_t) + valueSize;
	state int64_t kvBytesTarget = (int64_t)recordCountTarget * recordSize;

	printf("\nstoreType: %d\n", kvs->getType());
	printf("commitTarget: %d\n", commitTarget);
	printf("valueSize: %d\n", valueSize);
	printf("recordSize: %d\n", recordSize);
	printf("recordCountTarget: %d\n", recordCountTarget);
	printf("kvBytesTarget: %" PRId64 "\n", kvBytesTarget);

	state int64_t kvBytes = 0;
	state int64_t kvBytesTotal = 0;
	state int records = 0;
	state Future<Void> commit = Void();
	state std::string value = deterministicRandom()->randomAlphaNumeric(1e6);

	wait(kvs->init());

	state double intervalStart = timer();
	state double start = intervalStart;

	state std::function<void()> stats = [&]() {
		double elapsed = timer() - start;
		printf("Cumulative stats: %.2f seconds  %.2f MB keyValue bytes  %d records  %.2f MB/s  %.2f rec/s\r", elapsed,
		       kvBytesTotal / 1e6, records, kvBytesTotal / elapsed / 1e6, records / elapsed);
		fflush(stdout);
	};

	state uint64_t c = 0;
	state Key key = source.getKeyRef(sizeof(uint64_t));

	while (kvBytesTotal < kvBytesTarget) {
		wait(yield());
		*(uint64_t*)(key.end() - sizeof(uint64_t)) = bigEndian64(c);
		KeyValueRef kv(key, source.getValue(valueSize));
		kvs->set(kv);
		kvBytes += kv.expectedSize();
		++records;

		if (kvBytes >= commitTarget) {
			wait(commit);
			stats();
			commit = kvs->commit();
			kvBytesTotal += kvBytes;
			if (kvBytesTotal >= kvBytesTarget) {
				break;
			}
			kvBytes = 0;
		}
		++c;
	}

	wait(commit);
	stats();
	printf("\n");

	return Void();
}

Future<Void> closeKVS(IKeyValueStore* kvs) {
	Future<Void> closed = kvs->onClosed();
	kvs->close();
	return closed;
}

ACTOR Future<Void> doPrefixInsertComparison(int suffixSize, int valueSize, int recordCountTarget,
                                            bool usePrefixesInOrder, KVSource source) {
	VersionedBTree::counts.clear();

	deleteFile("test.redwood");
	wait(delay(5));
	state IKeyValueStore* redwood = openKVStore(KeyValueStoreType::SSD_REDWOOD_V1, "test.redwood", UID(), 0);
	wait(prefixClusteredInsert(redwood, suffixSize, valueSize, source, recordCountTarget, usePrefixesInOrder));
	wait(closeKVS(redwood));
	printf("\n");

	deleteFile("test.sqlite");
	deleteFile("test.sqlite-wal");
	wait(delay(5));
	state IKeyValueStore* sqlite = openKVStore(KeyValueStoreType::SSD_BTREE_V2, "test.sqlite", UID(), 0);
	wait(prefixClusteredInsert(sqlite, suffixSize, valueSize, source, recordCountTarget, usePrefixesInOrder));
	wait(closeKVS(sqlite));
	printf("\n");

	return Void();
}

TEST_CASE("!/redwood/performance/prefixSizeComparison") {
	state int suffixSize = 12;
	state int valueSize = 100;
	state int recordCountTarget = 100e6;
	state int usePrefixesInOrder = false;

	wait(doPrefixInsertComparison(suffixSize, valueSize, recordCountTarget, usePrefixesInOrder,
	                              KVSource({ { 10, 100000 } })));
	wait(doPrefixInsertComparison(suffixSize, valueSize, recordCountTarget, usePrefixesInOrder,
	                              KVSource({ { 16, 100000 } })));
	wait(doPrefixInsertComparison(suffixSize, valueSize, recordCountTarget, usePrefixesInOrder,
	                              KVSource({ { 32, 100000 } })));
	wait(doPrefixInsertComparison(suffixSize, valueSize, recordCountTarget, usePrefixesInOrder,
	                              KVSource({ { 4, 5 }, { 12, 1000 }, { 8, 5 }, { 8, 4 } })));

	return Void();
}

TEST_CASE("!/redwood/performance/sequentialInsert") {
	state int prefixLen = 30;
	state int valueSize = 100;
	state int recordCountTarget = 100e6;

	deleteFile("test.redwood");
	wait(delay(5));
	state IKeyValueStore* redwood = openKVStore(KeyValueStoreType::SSD_REDWOOD_V1, "test.redwood", UID(), 0);
	wait(sequentialInsert(redwood, prefixLen, valueSize, recordCountTarget));
	wait(closeKVS(redwood));
	printf("\n");

	return Void();
}
