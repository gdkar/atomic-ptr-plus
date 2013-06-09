/*
Copyright 2002-2005 Joseph W. Seigh 

Permission to use, copy, modify and distribute this software
and its documentation for any purpose and without fee is
hereby granted, provided that the above copyright notice
appear in all copies, that both the copyright notice and this
permission notice appear in supporting documentation.  I make
no representations about the suitability of this software for
any purpose. It is provided "as is" without express or implied
warranty.
---
*/

//------------------------------------------------------------------------------
// atomic_ptr -- C++ atomic lock-free reference counted smart pointer
//
// version -- 0.0.1 (pre-alpha)
//
//
// memory visibility:
//   atomic_ptr has memory release consistency for store into atomic_ptr
// types and dependent load consistency for loads from atomic_ptr types.
// There are no memory visibility guarantees for local_ptr except where
// local_ptr types are loaded from or stored into atomic_ptr types.
// Dereferencing an atomic_ptr has dependent load consistency.
// 
// The current implementation has two dependent load barriers when
// dereferencing an atomic_ptr type, one for the atomic_ptr_ref
// type and one for the referenced object itself.  This may be somewhat
// redundant but I'm leaving it that way for now until dependent load
// "memory barrier" semantics become better understood.
//
// Dropping a reference requires a release memory barrier if the
// resulting reference counts are non zero to prevent late stores
// into recycled objects.  If the resulting reference counts are zero
// an acquire memory barrier is required to prevent subsequent stores
// into the object before the count was dropped to zero.  The current
// implementation (atomic_ptr_ref::adjust) uses full synchronization
// handle both cases.  This may change to handle platforms where
// interlocked instructions don't have memory barriers built in.
// atomic_ptr_ref::adjust also handles adding references which do
// not require memory barriers.  A separate method without memory
// barriers may be created in the future.
//
// notes:
//   Dereferencing an atomic_ptr generates a local_ptr temp to guarantee
// that the object will not be deleted for the duration of the dereference.
// For operator ->, this will be transparent.  For operator * it will require
// a double ** to work.  This is a C++ quirk.
//
//   The recycling pool interface is experimental and may be subject
// to change.
//
//------------------------------------------------------------------------------

#ifndef _ATOMIC_PTR_H
#define _ATOMIC_PTR_H

#include <stdlib.h>
#include <atomix.h>

template<typename T> class atomic_ptr;
template<typename T> class local_ptr;
template<typename T> class atomic_ptr_ref;
//typedef const float ** (atomic_ptr<T>:: **)() atomic_null_t;

struct refcount {
	long	ecount;	// ephemeral count
	long	rcount;	// reference count
};

template<typename T> struct ref {
	long	ecount; // ephemeral count
	atomic_ptr_ref<T> *ptr;
};

typedef void (*pool_put_t)(void *);


//=============================================================================
// atomic_ptr_ref
//
//
//=============================================================================
template<typename T> class atomic_ptr_ref {
	friend class atomic_ptr<T>;
	friend class local_ptr<T>;

	public:

		typedef atomic_ptr_ref<T> _t;

		//typedef void (*pool_put_t)(atomic_ptr_ref<T> *);

	private:

		refcount	count;				// reference counts
		T *			ptr;				// ptr to actual object
		pool_put_t	pool;

	public:

		atomic_ptr_ref<T> * next;


		atomic_ptr_ref(T * p = 0) {
			count.ecount = 0;
			count.rcount = 1;
			ptr = p;
			pool = 0;
			next = 0;
		};


		~atomic_ptr_ref() {
			delete ptr;
		}

	private:

		//----------------------------------------------------------------------
		// adjust -- adjust refcounts
		//
		// For dropping references, a release membar is
		// required if reference counts do not go to zero to
		// prevent late stores into deleted storage. An
		// acquire membar is required if the reference counts
		// go to zero to prevent early stores into a live object.
		// _sync takes care of both without having to finese
		// special cases for _rel and _acq.
		//
		// Adding references does not require membars.  This may
		// be broken out into a separate routine in the future.
		//----------------------------------------------------------------------
		int adjust(long xephemeralCount, long xreferenceCount) {
			refcount oldval, newval;

			oldval.ecount = count.ecount;
			oldval.rcount = count.rcount;
			do {
				newval.ecount = oldval.ecount + xephemeralCount;
				newval.rcount = oldval.rcount + xreferenceCount;

			}
			while (atomic_cas_sync(&count, &oldval, &newval) == 0);

			return (newval.ecount == 0 && newval.rcount == 0) ? 0 : 1;
		}

}; // class atomic_ptr_ref


//=============================================================================
// local_ptr
//
//
//=============================================================================
template<typename T> class local_ptr {
	friend class atomic_ptr<T>;
	public:

		local_ptr(T * obj = 0) {
			if (obj != 0) {
				refptr = new atomic_ptr_ref<T>(obj);
				refptr->count.ecount = 1;
				refptr->count.rcount = 0;
			}
			else
				refptr = 0;
		}

		local_ptr(const local_ptr<T> & src) {
			if ((refptr = src.refptr) != 0)
				refptr->adjust(+1, 0);
		}

		local_ptr(atomic_ptr<T> & src) {
			refptr = src.getrefptr();
		}

		// recycled ref object
		local_ptr(atomic_ptr_ref<T>  * src) {
			refptr = src;
			if (refptr != 0) {
				refptr->count.ecount = 1;
				refptr->count.rcount = 0;
			}
			// pool unchanged
		}

		~local_ptr() {
			if (refptr != 0 && refptr->adjust(-1, 0) == 0) {
				if (refptr->pool == 0)
					delete refptr;
				else
					refptr->pool(refptr);		// recyle to pool
			}
		}
		
		local_ptr<T> & operator = (T * obj) {
			local_ptr<T> temp(obj);
			swap(temp);	// non-atomic
			return *this;
		}

		local_ptr<T> & operator = (local_ptr<T> & src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			return *this;
		}

		local_ptr<T> & operator = (atomic_ptr<T> & src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			return *this;
		}


		T * get() {
			return (refptr != 0) ? atomic_load_depends(&(refptr->ptr)) : (T *)0;
		}

		T * operator -> () { return get(); }
		T & operator * () { return *get(); }
		//operator T* () { return  get(); }

		bool operator == (T * rhd) { return (rhd == get() ); }
		bool operator != (T * rhd) { return (rhd != get() ); }

		// refptr == rhd.refptr  iff  refptr->ptr == rhd.refptr->ptr
		bool operator == (local_ptr<T> & rhd) { return (refptr == rhd.refptr);}
		bool operator != (local_ptr<T> & rhd) { return (refptr != rhd.refptr);}
		bool operator == (atomic_ptr<T> & rhd) { return (refptr == rhd.xxx.ptr);}
		bool operator != (atomic_ptr<T> & rhd) { return (refptr != rhd.xxx.ptr);}

		//-----------------------------------------------------------------
		// set/get recycle pool methods
		//-----------------------------------------------------------------

		void recycle(atomic_ptr_ref<T> * src) {
			local_ptr<T> temp(src);
			swap(temp);	// non-atomic
			//return *this;
		}

		void setPool(pool_put_t pool) {
			refptr->pool = pool;	// set pool
		}

		pool_put_t getPool() {
			return refptr->pool;
		}


	private:
		void * operator new (size_t) {}		// auto only

		atomic_ptr_ref<T> * refptr;

		inline void swap(local_ptr & other) { // non-atomic swap
			atomic_ptr_ref<T> * temp;
			temp = refptr;
			refptr = other.refptr;
			other.refptr = temp;
		}


}; // class local_ptr


template<typename T> inline bool operator == (int lhd, local_ptr<T> & rhd)
	{ return ((T *)lhd == rhd); }

template<typename T> inline bool operator != (int lhd, local_ptr<T> & rhd)
	{ return ((T *)lhd != rhd); }

template<typename T> inline bool operator == (T * lhd, local_ptr<T> & rhd)
	{ return (rhd == lhd); }

template<typename T> inline bool operator != (T * lhd, local_ptr<T> & rhd)
	{ return (rhd != lhd); }


//=============================================================================
// atomic_ptr
//
//
//=============================================================================
template<typename T> class atomic_ptr {
	friend class local_ptr<T>;

	protected:
		ref<T>  xxx;

	public:

		atomic_ptr(T * obj = 0) {
			xxx.ecount = 0;
			if (obj != 0) {
				xxx.ptr = new atomic_ptr_ref<T>(obj);
			}
			else
				xxx.ptr = 0;
		}

		atomic_ptr(local_ptr<T> & src) {	// copy constructor
			xxx.ecount = 0;
			if ((xxx.ptr = src.refptr) != 0)
				xxx.ptr->adjust(0, +1);
		}

		atomic_ptr(atomic_ptr<T> & src) {  // copy constructor
			xxx.ecount = 0;
			xxx.ptr = src.getrefptr();	// atomic 

			// adjust link count
			if (xxx.ptr != 0)
				xxx.ptr->adjust(-1, +1);	// atomic
		}

		// recycled ref objects
		atomic_ptr(atomic_ptr_ref<T> * src) { // copy constructor
			if (src != 0) {
				src->count.ecount = 0;
				src->count.rcount = 1;
			}
			xxx.ecount = 0;
			xxx.ptr = src;	// atomic 
		}


		~atomic_ptr() {					// destructor
			// membar.release
			if (xxx.ptr != 0 && xxx.ptr->adjust(xxx.ecount, -1) == 0) {
				// membar.acquire
				if (xxx.ptr->pool == 0)
					delete xxx.ptr;
				else
					xxx.ptr->pool(xxx.ptr);
			}
		}

		atomic_ptr & operator = (T * obj) {
			atomic_ptr<T> temp(obj);
			swap(temp);					// atomic
			return *this;
		}

		atomic_ptr & operator = (local_ptr<T> & src) {
			atomic_ptr<T> temp(src);
			swap(temp);					// atomic
			return *this;
		}

		atomic_ptr & operator = (atomic_ptr<T> & src) {
			atomic_ptr<T> temp(src);
			swap(temp);					// atomic
			return *this;
		}

		
		//-----------------------------------------------------------------
		// generate local temp ptr to guarantee validity of ptr
		// during lifetime of expression. temp is dtor'd after
		// expression has finished execution.
		//-----------------------------------------------------------------
		inline local_ptr<T> operator -> () { return local_ptr<T>(*this); }
		inline local_ptr<T> operator * () { return local_ptr<T>(*this); }

		bool operator == (T * rhd) {
			if (rhd == 0)
				return (xxx.ptr == 0);
			else
				return (local_ptr<T>(*this) == rhd);
		}

		bool operator != (T * rhd) {
			if (rhd == 0)
				return (xxx.ptr != 0);
			else
				return (local_ptr<T>(*this) != rhd);
		}

		bool operator == (local_ptr<T> & rhd) {return (local_ptr<T>(*this) == rhd); }
		bool operator != (local_ptr<T> & rhd) {return (local_ptr<T>(*this) != rhd); }
		bool operator == (atomic_ptr<T> & rhd) {return (local_ptr<T>(*this) == local_ptr<T>(rhd)); }
		bool operator != (atomic_ptr<T> & rhd) {return (local_ptr<T>(*this) != local_ptr<T>(rhd)); }

		bool cas(local_ptr<T> cmp, atomic_ptr<T> xchg) {
			ref<T> temp;
			bool rc = false;

			temp.ecount = xxx.ecount;
			temp.ptr = cmp.refptr;

			// membar.release
			do {
				if (atomic_cas_rel(&xxx, &temp, &xchg.xxx) != 0) {
					xchg.xxx = temp;
					rc = true;
					break;
				}

			}
			while (cmp.refptr == temp.ptr);

			return rc;
		}

		//-----------------------------------------------------------------
		// recycle pool methods
		//-----------------------------------------------------------------

		void recyle(atomic_ptr_ref<T> * src) {
			atomic_ptr<T> temp(src);
			swap(temp);	// atomic
			//return *this;
		}

	//protected:
		// atomic
		void swap(atomic_ptr<T> & obj) {	// obj is local & non-shared
			ref<T> temp;

			temp.ecount = xxx.ecount;
			temp.ptr = xxx.ptr;

			// membar.release
			while(atomic_cas_rel(&xxx, &temp, &obj.xxx) == 0);

			obj.xxx.ecount = temp.ecount;
			obj.xxx.ptr = temp.ptr;
		}

	private:

		// atomic
		atomic_ptr_ref<T> * getrefptr() {
			ref<T> oldval, newval;

			oldval.ecount = xxx.ecount;
			oldval.ptr = xxx.ptr;

			do {
				newval.ecount = oldval.ecount + 1;
				newval.ptr = oldval.ptr;
			}
			while (atomic_cas(&xxx, &oldval, &newval) == 0);

			return atomic_load_depends(&oldval.ptr);
		}

}; // class atomic_ptr

template<typename T> inline bool operator == (int lhd, atomic_ptr<T> & rhd)
	{ return ((T *)lhd == rhd); }

template<typename T> inline bool operator != (int lhd, atomic_ptr<T> & rhd)
	{ return ((T *)lhd != rhd); }

template<typename T> inline bool operator == (T * lhd, atomic_ptr<T> & rhd)
	{ return (rhd == lhd); }

template<typename T> inline bool operator != (T * lhd, atomic_ptr<T> & rhd)
	{ return (rhd != lhd); }

#endif // _ATOMIC_PTR_H


/*-*/
