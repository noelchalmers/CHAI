// ---------------------------------------------------------------------
// Copyright (c) 2016-2018, Lawrence Livermore National Security, LLC. All
// rights reserved.
//
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of CHAI.
//
// LLNL-CODE-705877
//
// For details, see https:://github.com/LLNL/CHAI
// Please also see the NOTICE and LICENSE files.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the
//   distribution.
//
// - Neither the name of the LLNS/LLNL nor the names of its contributors
//   may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
// OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
// AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
// WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
// ---------------------------------------------------------------------

#ifndef MANAGED_PTR_H_
#define MANAGED_PTR_H_

#include "chai/config.hpp"

#ifndef CHAI_DISABLE_RM
#include "chai/ArrayManager.hpp"
#endif

#include "chai/ChaiMacros.hpp"
#include "chai/ExecutionSpaces.hpp"
#include "chai/ManagedArray.hpp"
#include "chai/Types.hpp"

// Standard libary headers
#include <cstddef>
#include <functional>

#ifdef __CUDACC__

inline void gpuErrorCheck(cudaError_t code, const char *file, int line, bool abort=true)
{
   if (code != cudaSuccess) {
      fprintf(stderr, "[CHAI] GPU Error: %s %s %d\n", cudaGetErrorString(code), file, line);
      if (abort) {
         exit(code);
      }
   }
}

#if DEBUG
#define GPU_ERROR_CHECK(code) { gpuErrorCheck((code), __FILE__, __LINE__); }
#else
#define GPU_ERROR_CHECK(code) code
#endif

inline void debug_cudaDeviceSynchronize() {
#if DEBUG
   GPU_ERROR_CHECK(cudaDeviceSynchronize());
#endif
}

#endif // __CUDACC__

namespace chai {
   namespace detail {
#ifdef __CUDACC__
      template <typename T>
      __global__ void destroy_on_device(T* gpuPointer);
#endif
   }

   struct managed_ptr_record {
      managed_ptr_record() :
         m_num_references(1),
         m_callback()
      {
      }

      managed_ptr_record(std::function<bool(Action, ExecutionSpace, void*)> callback) :
         m_num_references(1),
         m_callback(callback)
      {
      }

      size_t use_count() {
         return m_num_references;
      }

      void addReference() {
         m_num_references++;
      }

      void removeReference() {
         m_num_references--;
      }

      ExecutionSpace getLastSpace() {
         return m_last_space;
      }

      void set_callback(std::function<bool(Action, ExecutionSpace, void*)> callback) {
         m_callback = callback;
      }

      size_t m_num_references = 1; /// The reference counter
      ExecutionSpace m_last_space = NONE; /// The last space executed in
      std::function<bool(Action, ExecutionSpace, void*)> m_callback; /// Callback to handle events
   };

   ///
   /// @class managed_ptr<T>
   /// @author Alan Dayton
   ///
   /// This wrapper stores both host and device pointers so that polymorphism can be
   ///    used in both contexts with a single API. It is modeled after std::shared_ptr,
   ///    so it does reference counting and automatically cleans up when the last
   ///    reference is destroyed. If we ever do multi-threading on the CPU, locking will
   ///    need to be added to the reference counter.
   /// The make_managed and make_managed_from_factory functions call new on both the
   ///    host and device so that polymorphism is valid in both contexts. Simply copying
   ///    an object to the device will not copy the vtable, so new must be called on
   ///    the device.
   ///
   /// Usage Requirements:
   ///    Methods that can be called on both the host and device must be declared
   ///       with the __host__ __device__ specifiers. This includes constructors
   ///       and destructors. Furthermore, destructors of base and child classes
   ///       must all be declared virtual.
   ///    This wrapper does NOT automatically sync the device object if the host object
   ///       is updated and vice versa. If you wish to keep both instances in sync,
   ///       you must explicitly modify the object in both the host context and the
   ///       device context.
   ///    Raw array members of T need to be initialized correctly with a host or
   ///       device pointer. If a ManagedArray is passed to the make_managed or
   ///       make_managed_from_factory methods in place of a raw array, it will be
   ///       cast to the appropriate host or device pointer when passed to T's
   ///       constructor on the host and on the device. If it is desired that these
   ///       host and device pointers be kept in sync, define a callback that maintains
   ///       a copy of the ManagedArray and upon the ACTION_MOVE event calls the copy
   ///       constructor of that ManagedArray.
   ///    If a raw array is passed to make_managed, accessing that member will be
   ///       valid only in the correct context. To prevent the accidental use of that
   ///       member in the wrong context, any methods that access it should be __host__
   ///       only or __device__ only. Special care should be taken when passing raw
   ///       arrays as arguments to member functions.
   ///    The same restrictions for raw array members also apply to raw pointer members.
   ///       A managed_ptr can be passed to the make_managed or make_managed_from_factory
   ///       methods in place of a raw pointer, and the host constructor of T will
   ///       be given the extracted host pointer, and likewise the device constructor
   ///       of T will be given the extracted device pointer. It is recommended that
   ///       a callback is defined that maintains a copy of the managed_ptr so that
   ///       the raw pointers are not accidentally destroyed prematurely (since
   ///       managed_ptr does reference counting). It is also recommended that the
   ///       callback calls the copy constructor of the managed_ptr on the ACTION_MOVE
   ///       event so that the ACTION_MOVE event is triggered also for the inner
   ///       managed_ptr.
   ///    Again, if a raw pointer is passed to make_managed, accessing that member will
   ///       only be valid in the correct context. Take care when passing raw pointers
   ///       as arguments to member functions.
   ///    Be aware that only the debug version of CHAI will check for GPU errors. So
   ///       if you are seeing strange behavior and/or your code crashes in the
   ///       constructor/destructor of T, then build CHAI as debug to see what is
   ///       going on. For example, the constructor of T might run out of per-thread
   ///       stack space on the GPU. If that happens, you can increase the device
   ///       limit of per-thread stack space. Alternatively, you could add a call
   ///       to cudaDeviceSynchronize after calling make_managed and check the return
   ///       code of cudaDeviceSynchronize.
   ///
   template <typename T>
   class managed_ptr {
      public:
         using element_type = T;

         ///
         /// @author Alan Dayton
         ///
         /// Default constructor.
         /// Initializes the reference count to 0.
         ///
         CHAI_HOST_DEVICE constexpr managed_ptr() noexcept {}

         ///
         /// @author Alan Dayton
         ///
         /// Construct from nullptr.
         /// Initializes the reference count to 0.
         ///
         CHAI_HOST_DEVICE constexpr managed_ptr(std::nullptr_t) noexcept {}

         ///
         /// @author Alan Dayton
         ///
         /// Constructs a managed_ptr from the given pointers. U* must be convertible
         ///    to T*.
         ///
         /// @pre spaces.size() == pointers.size()
         ///
         /// @param[in] spaces A list of execution spaces
         /// @param[in] pointers A list of pointers to take ownership of
         ///
         template <typename U>
         managed_ptr(std::initializer_list<ExecutionSpace> spaces,
                     std::initializer_list<U*> pointers) :
            m_cpu_pointer(nullptr),
            m_gpu_pointer(nullptr),
            m_pointer_record(new managed_ptr_record())
         {
            static_assert(std::is_convertible<U*, T*>::value,
                          "U* must be convertible to T*.");

            // TODO: In c++14 convert to a static_assert
            if (spaces.size() != pointers.size()) {
               printf("[CHAI] WARNING: The number of spaces is different than the number of pointers given!\n");
            }

            int i = 0;

            for (const auto& space : spaces) {
               switch (space) {
                  case CPU:
                     m_cpu_pointer = pointers.begin()[i++];
                     break;
#ifdef __CUDACC__
                  case GPU:
                     m_gpu_pointer = pointers.begin()[i++];
                     break;
#endif
                  default:
                     ++i;
                     printf("[CHAI] WARNING: Execution space not supported by chai::managed_ptr!\n");
                     break;
               }
            }
         }

         ///
         /// @author Alan Dayton
         ///
         /// Constructs a managed_ptr from the given pointers and callback function.
         ///    U* must be convertible to T*.
         ///
         /// @pre spaces.size() == pointers.size()
         ///
         /// @param[in] spaces A list of execution spaces
         /// @param[in] pointers A list of pointers to take ownership of
         /// @param[in] callback The user defined callback to call on trigger events
         ///
         template <typename U>
         CHAI_HOST managed_ptr(std::initializer_list<ExecutionSpace> spaces,
                               std::initializer_list<U*> pointers,
                               std::function<bool(Action, ExecutionSpace, void*)> callback) :
            m_cpu_pointer(nullptr),
            m_gpu_pointer(nullptr),
            m_pointer_record(new managed_ptr_record(callback))
         {
            static_assert(std::is_convertible<U*, T*>::value,
                          "U* must be convertible to T*.");

            // TODO: In c++14 convert to a static_assert
            if (spaces.size() != pointers.size()) {
               printf("[CHAI] WARNING: The number of spaces is different than the number of pointers given.\n");
            }

            int i = 0;

            for (const auto& space : spaces) {
               switch (space) {
                  case CPU:
                     m_cpu_pointer = pointers.begin()[i++];
                     break;
#ifdef __CUDACC__
                  case GPU:
                     m_gpu_pointer = pointers.begin()[i++];
                     break;
#endif
                  default:
                     ++i;
                     printf("[CHAI] WARNING: Execution space not supported by chai::managed_ptr!\n");
                     break;
               }
            }
         }

         ///
         /// @author Alan Dayton
         ///
         /// Copy constructor.
         /// Constructs a copy of the given managed_ptr, increases the reference count,
         ///    and if the execution space is different, calls the user defined callback
         ///    with ACTION_MOVE for each of the execution spaces.
         ///
         /// @param[in] other The managed_ptr to copy
         ///
         CHAI_HOST_DEVICE managed_ptr(const managed_ptr& other) noexcept :
            m_cpu_pointer(other.m_cpu_pointer),
            m_gpu_pointer(other.m_gpu_pointer),
            m_pointer_record(other.m_pointer_record)
         {
#ifndef __CUDA_ARCH__
            addReference();
            move();
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Converting constructor.
         /// Constructs a copy of the given managed_ptr, increases the reference count,
         ///    and if the execution space is different, calls the user defined callback
         ///    with ACTION_MOVE for each of the execution spaces. U* must be convertible
         ///    to T*.
         ///
         /// @param[in] other The managed_ptr to copy
         ///
         template <typename U>
         CHAI_HOST_DEVICE managed_ptr(const managed_ptr<U>& other) noexcept :
            m_cpu_pointer(other.m_cpu_pointer),
            m_gpu_pointer(other.m_gpu_pointer),
            m_pointer_record(other.m_pointer_record)
         {
            static_assert(std::is_convertible<U*, T*>::value,
                          "U* must be convertible to T*.");

#ifndef __CUDA_ARCH__
            addReference();
            move();
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Aliasing constructor.
         /// Has the same ownership information as other, but holds different pointers.
         ///
         /// @pre spaces.size() == pointers.size()
         ///
         /// @param[in] other The managed_ptr to copy ownership information from
         /// @param[in] spaces A list of execution spaces
         /// @param[in] pointers A list of pointers to maintain a reference to
         ///
         template <typename U>
         CHAI_HOST managed_ptr(const managed_ptr<U>& other,
                               std::initializer_list<ExecutionSpace> spaces,
                               std::initializer_list<T*> pointers) noexcept :
            m_pointer_record(other.m_pointer_record)
         {
            // TODO: In c++14 convert to a static_assert
            if (spaces.size() != pointers.size()) {
               printf("[CHAI] WARNING: The number of spaces is different than the number of pointers given.\n");
            }

            int i = 0;

            for (const auto& space : spaces) {
               switch (space) {
                  case CPU:
                     m_cpu_pointer = pointers.begin()[i++];
                     break;
#ifdef __CUDACC__
                  case GPU:
                     m_gpu_pointer = pointers.begin()[i++];
                     break;
#endif
                  default:
                     ++i;
                     printf("[CHAI] WARNING: Execution space not supported by chai::managed_ptr!\n");
                     break;
               }
            }

            addReference();
            move();
         }

         ///
         /// @author Alan Dayton
         ///
         /// Destructor. Decreases the reference count and if this is the last reference,
         ///    clean up.
         ///
         CHAI_HOST_DEVICE ~managed_ptr() {
#ifdef __CUDACC__
            // This trick came from Max Katz at Nvidia.
            // Taking the address of this kernel ensures that it gets instantiated
            // by the compiler and can be used within __CUDA_ARCH__. Without this,
            // calling destroy_on_device within the confines of __CUDA_ARCH__ will
            // always fail with error code 0x8 (invalid device function).
            // https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#restrictions
            // From the CUDA Programming Guide Restrictions:
            // "If a __global__ function template is instantiated and launched from
            // the host, then the function template must be instantiated with the
            // same template arguments irrespective of whether __CUDA_ARCH__ is
            // defined and regardless of the value of __CUDA_ARCH__."
            (void) &detail::destroy_on_device<T>;
#endif

#ifndef __CUDA_ARCH__
            removeReference();
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Copy assignment operator.
         /// Copies the given managed_ptr and increases the reference count.
         ///
         /// @param[in] other The managed_ptr to copy
         ///
         CHAI_HOST_DEVICE managed_ptr& operator=(const managed_ptr& other) noexcept {
            if (this != &other) {
#ifndef __CUDA_ARCH__
               removeReference();
#endif

               m_cpu_pointer = other.m_cpu_pointer;
               m_gpu_pointer = other.m_gpu_pointer;
               m_pointer_record = other.m_pointer_record;

#ifndef __CUDA_ARCH__
               addReference();
               move();
#endif
            }

            return *this;
         }

         ///
         /// @author Alan Dayton
         ///
         /// Conversion copy assignment operator.
         /// Copies the given managed_ptr and increases the reference count.
         ///    U* must be convertible to T*.
         ///
         /// @param[in] other The managed_ptr to copy
         ///
         template <typename U>
         CHAI_HOST_DEVICE managed_ptr& operator=(const managed_ptr<U>& other) noexcept {
            static_assert(std::is_convertible<U*, T*>::value,
                          "U* must be convertible to T*.");

#ifndef __CUDA_ARCH__
            removeReference();
#endif

            m_cpu_pointer = other.m_cpu_pointer;
            m_gpu_pointer = other.m_gpu_pointer;
            m_pointer_record = other.m_pointer_record;

#ifndef __CUDA_ARCH__
            addReference();
            move();
#endif

            return *this;
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns the CPU or GPU pointer depending on the calling context.
         ///
         CHAI_HOST_DEVICE inline T* get() const {
#ifndef __CUDA_ARCH__
            move();
            return m_cpu_pointer;
#else
            return m_gpu_pointer;
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns the pointer corresponding to the given execution space.
         ///
         /// @param[in] space The execution space
         /// @param[in] move Whether or not to trigger the move event (default is true)
         ///
         CHAI_HOST inline T* get(const ExecutionSpace space, const bool move=true) const {
            if (move) {
               this->move();
            }

            switch (space) {
               case CPU:
                  return m_cpu_pointer;
#ifdef __CUDACC__
               case GPU:
                  return m_gpu_pointer;
#endif
               default:
                  return nullptr;
            }
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns the CPU or GPU pointer depending on the calling context.
         ///
         CHAI_HOST_DEVICE inline T* operator->() const {
#ifndef __CUDA_ARCH__
            return m_cpu_pointer;
#else
            return m_gpu_pointer;
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns the CPU or GPU reference depending on the calling context.
         ///
         CHAI_HOST_DEVICE inline T& operator*() const {
#ifndef __CUDA_ARCH__
            return *m_cpu_pointer;
#else
            return *m_gpu_pointer;
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns the number of managed_ptrs owning these pointers.
         ///
         CHAI_HOST std::size_t use_count() const {
            if (m_pointer_record) {
               return m_pointer_record->use_count();
            }
            else {
               return 0;
            }
         }

         ///
         /// @author Alan Dayton
         ///
         /// Returns true if the contained pointer is not nullptr, false otherwise.
         ///
         CHAI_HOST_DEVICE inline explicit operator bool() const noexcept {
            return get() != nullptr;
         }

         ///
         /// @author Alan Dayton
         ///
         /// Sets the callback, which can be used to handle specific actions.
         /// ACTION_MOVE can be used to call the copy constructor for ManagedArrays.
         /// ACTION_FREE can be used to provide a custom deleter operation. Use
         ///    ExecutionSpace::NONE if freeing anything other than the actual object
         ///    pointers.
         ///
         /// @param[in] callback The callback to call when certain actions occur
         ///
         CHAI_HOST void set_callback(std::function<bool(Action, ExecutionSpace, void*)> callback) {
            if (m_pointer_record) {
               m_pointer_record->set_callback(callback);
            }
            else {
               printf("[CHAI] WARNING: No callback is allowed for managed_ptr that does not contain a valid pointer (i.e. the default or nullptr constructor was used)!\n");
            }
         }

      private:
         T* m_cpu_pointer = nullptr; /// The CPU pointer
         T* m_gpu_pointer = nullptr; /// The GPU pointer
         managed_ptr_record* m_pointer_record = nullptr; /// The pointer record

         /// Needed for the converting constructor
         template <typename U>
         friend class managed_ptr;

         /// Needed to use the make_managed API
         template <typename U,
                   typename... Args>
         friend CHAI_HOST managed_ptr<U> make_managed(Args... args);

         ///
         /// @author Alan Dayton
         ///
         /// If the execution space has changed, calls the user provided callback
         ///    with the ACTION_MOVE event.
         ///
         CHAI_HOST void move() const {
#ifndef CHAI_DISABLE_RM
            if (m_pointer_record) {
               ExecutionSpace newSpace = ArrayManager::getInstance()->getExecutionSpace();

               if (newSpace != NONE && newSpace != m_pointer_record->getLastSpace()) {
                  m_pointer_record->m_last_space = newSpace;

                  if (m_pointer_record->m_callback) {
                     for (int space = NONE; space < NUM_EXECUTION_SPACES; ++space) {
                        ExecutionSpace execSpace = static_cast<ExecutionSpace>(space);

                        T* pointer = get(execSpace, false);

                        using T_non_const = typename std::remove_const<T>::type;

                        // We can use const_cast because can managed_ptr can only
                        // be constructed with non const pointers.
                        T_non_const* temp = const_cast<T_non_const*>(pointer);

                        void* voidPointer = static_cast<void*>(temp);

                        m_pointer_record->m_callback(ACTION_MOVE, execSpace, voidPointer);
                     }
                  }
               }
            }
#endif
         }

         ///
         /// @author Alan Dayton
         ///
         /// Increments the reference count and calls the copy constructor to
         ///    trigger data movement.
         ///
         CHAI_HOST void addReference() {
            if (m_pointer_record) {
               m_pointer_record->addReference();
            }
         }

         ///
         /// @author Alan Dayton
         ///
         /// Decrements the reference counter. If the resulting number of references
         ///    is 0, clean up the object.
         ///
         CHAI_HOST void removeReference() {
            if (m_pointer_record) {
               m_pointer_record->removeReference();

               if (m_pointer_record->use_count() == 0) {
                  if (m_pointer_record->m_callback) {
                     // Destroy device pointer first to take advantage of asynchrony
                     for (int space = NUM_EXECUTION_SPACES-1; space >= NONE; --space) {
                        ExecutionSpace execSpace = static_cast<ExecutionSpace>(space);
                        T* pointer = get(execSpace, false);

                        using T_non_const = typename std::remove_const<T>::type;

                        // We can use const_cast because can managed_ptr can only
                        // be constructed with non const pointers.
                        T_non_const* temp = const_cast<T_non_const*>(pointer);
                        void* voidPointer = static_cast<void*>(temp);

                        if (!m_pointer_record->m_callback(ACTION_FREE,
                                                          execSpace,
                                                          voidPointer)) {
                           switch (execSpace) {
                              case CPU:
                                 delete pointer;
                                 break;
#ifdef __CUDACC__
                              case GPU:
                              {
                                 if (pointer) {
                                    detail::destroy_on_device<<<1, 1>>>(temp);
                                    debug_cudaDeviceSynchronize();
                                 }

                                 break;
                              }
#endif
                              default:
                                 break;
                           }
                        }
                     }
                  }
                  else {
                     // Destroy device pointer first to take advantage of asynchrony
                     for (int space = NUM_EXECUTION_SPACES-1; space >= NONE; --space) {
                        ExecutionSpace execSpace = static_cast<ExecutionSpace>(space);
                        T* pointer = get(execSpace, false);

                        switch (execSpace) {
                           case CPU:
                              delete pointer;
                              break;
#ifdef __CUDACC__
                           case GPU:
                           {
                              if (pointer) {
                                 detail::destroy_on_device<<<1, 1>>>(pointer);
                                 debug_cudaDeviceSynchronize();
                              }

                              break;
                           }
#endif
                           default:
                              break;
                        }
                     }
                  }

                  delete m_pointer_record;
               }
            }
         }

   };

   namespace detail {
      ///
      /// @author Alan Dayton
      ///
      /// This implementation of getRawPointers handles every non-CHAI type.
      ///
      /// @param[in] arg The non-CHAI type, which will simply be returned
      ///
      /// @return arg
      ///
      template <typename T>
      CHAI_HOST_DEVICE T getRawPointers(T arg) {
         return arg;
      }

      ///
      /// @author Alan Dayton
      ///
      /// This implementation of getRawPointers handles the CHAI ManagedArray type.
      ///
      /// @param[in] arg The ManagedArray from which to extract a raw pointer
      ///
      /// @return arg cast to a raw pointer
      ///
      template <typename T>
      CHAI_HOST_DEVICE T* getRawPointers(ManagedArray<T> arg) {
         return (T*) arg;
      }

      ///
      /// @author Alan Dayton
      ///
      /// This implementation of getRawPointers handles the CHAI managed_ptr type.
      /// The managed_ptr type is not implicitly convertible to a raw pointer, so
      ///    when using the make_managed API, it is necessary to pull the raw pointers
      ///    out of the managed_ptr.
      ///
      /// @param[in] arg The managed_ptr from which to extract a raw pointer
      ///
      /// @return a raw pointer acquired from arg
      ///
      template <typename T>
      CHAI_HOST_DEVICE T* getRawPointers(managed_ptr<T> arg) {
         return arg.get();
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new object on the host and returns a pointer to it.
      /// This implementation of new_on_host is called when no arguments need to be
      ///    converted to raw pointers.
      ///
      /// @param[in] args The arguments to T's constructor
      ///
      /// @return a pointer to the new object on the host
      ///
      template <typename T,
                typename... Args,
                typename std::enable_if<std::is_constructible<T, Args...>::value, int>::type = 0>
      CHAI_HOST T* new_on_host(Args&&... args) {
         return new T(args...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new object on the host and returns a pointer to it.
      /// This implementation of new_on_host is called when arguments do need to be
      ///    converted to raw pointers.
      ///
      /// @param[in] args The arguments to T's constructor
      ///
      /// @return a pointer to the new object on the host
      ///
      template <typename T,
                typename... Args,
                typename std::enable_if<!std::is_constructible<T, Args...>::value, int>::type = 0>
      CHAI_HOST T* new_on_host(Args&&... args) {
         return new T(getRawPointers(args)...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new T on the host.
      /// Sets the execution space to the CPU so that ManagedArrays and managed_ptrs
      ///    are moved to the host as necessary.
      ///
      /// @param[in]  args The arguments to T's constructor
      ///
      /// @return The host pointer to the new T
      ///
      template <typename T,
                typename... Args>
      CHAI_HOST T* make_on_host(Args&&... args) {
#ifndef CHAI_DISABLE_RM
         // Get the ArrayManager and save the current execution space
         chai::ArrayManager* arrayManager = chai::ArrayManager::getInstance();
         ExecutionSpace currentSpace = arrayManager->getExecutionSpace();

         // Set the execution space so that ManagedArrays and managed_ptrs
         // are handled properly
         arrayManager->setExecutionSpace(CPU);
#endif

         // Create on the host
         T* cpuPointer = detail::new_on_host<T>(args...);

#ifndef CHAI_DISABLE_RM
         // Set the execution space back to the previous value
         arrayManager->setExecutionSpace(currentSpace);
#endif

         // Return the CPU pointer
         return cpuPointer;
      }

      ///
      /// @author Alan Dayton
      ///
      /// Calls a factory method to create a new object on the host.
      /// Sets the execution space to the CPU so that ManagedArrays and managed_ptrs
      ///    are moved to the host as necessary.
      ///
      /// @param[in]  f    The factory method
      /// @param[in]  args The arguments to the factory method
      ///
      /// @return The host pointer to the new object
      ///
      template <typename T,
                typename F,
                typename... Args>
      CHAI_HOST T* make_on_host_from_factory(F f, Args&&... args) {
#ifndef CHAI_DISABLE_RM
         // Get the ArrayManager and save the current execution space
         chai::ArrayManager* arrayManager = chai::ArrayManager::getInstance();
         ExecutionSpace currentSpace = arrayManager->getExecutionSpace();

         // Set the execution space so that ManagedArrays and managed_ptrs
         // are handled properly
         arrayManager->setExecutionSpace(CPU);
#endif

         // Create the object on the device
         T* cpuPointer = f(args...);

#ifndef CHAI_DISABLE_RM
         // Set the execution space back to the previous value
         arrayManager->setExecutionSpace(currentSpace);
#endif

         // Return the GPU pointer
         return cpuPointer;
      }

#ifdef __CUDACC__
      ///
      /// @author Alan Dayton
      ///
      /// Creates a new object on the device and returns a pointer to it.
      /// This implementation of new_on_device is called when no arguments need to be
      ///    converted to raw pointers.
      ///
      /// @param[in] args The arguments to T's constructor
      ///
      /// @return a pointer to the new object on the device
      ///
      template <typename T,
                typename... Args,
                typename std::enable_if<std::is_constructible<T, Args...>::value, int>::type = 0>
      CHAI_DEVICE void new_on_device(T** gpuPointer, Args&&... args) {
         *gpuPointer = new T(args...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new object on the device and returns a pointer to it.
      /// This implementation of new_on_device is called when arguments do need to be
      ///    converted to raw pointers.
      ///
      /// @param[in] args The arguments to T's constructor
      ///
      /// @return a pointer to the new object on the device
      ///
      template <typename T,
                typename... Args,
                typename std::enable_if<!std::is_constructible<T, Args...>::value, int>::type = 0>
      CHAI_DEVICE void new_on_device(T** gpuPointer, Args&&... args) {
         *gpuPointer = new T(getRawPointers(args)...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new T on the device.
      ///
      /// @param[out] gpuPointer Used to return the device pointer to the new T
      /// @param[in]  args The arguments to T's constructor
      ///
      /// @note Cannot capture argument packs in an extended device lambda,
      ///       so explicit kernel is needed.
      ///
      template <typename T,
                typename... Args>
      __global__ void make_on_device(T** gpuPointer, Args... args)
      {
         new_on_device(gpuPointer, args...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new object on the device by calling the given factory method.
      ///
      /// @param[out] gpuPointer Used to return the device pointer to the new object
      /// @param[in]  f The factory method (must be a __device__ or __host__ __device__
      ///                method
      /// @param[in]  args The arguments to the factory method
      ///
      /// @note Cannot capture argument packs in an extended device lambda,
      ///       so explicit kernel is needed.
      ///
      template <typename T,
                typename F,
                typename... Args>
      __global__ void make_on_device_from_factory(T** gpuPointer, F f, Args... args)
      {
         *gpuPointer = f(args...);
      }

      ///
      /// @author Alan Dayton
      ///
      /// Destroys the device pointer.
      ///
      /// @param[out] gpuPointer The device pointer to call delete on
      ///
      template <typename T>
      __global__ void destroy_on_device(T* gpuPointer)
      {
         if (gpuPointer) {
            delete gpuPointer;
         }
      }

      ///
      /// @author Alan Dayton
      ///
      /// Creates a new T on the device.
      ///
      /// @param[in]  args The arguments to T's constructor
      ///
      /// @return The device pointer to the new T
      ///
      template <typename T,
                typename... Args>
      CHAI_HOST T* make_on_device(Args... args) {
#ifndef CHAI_DISABLE_RM
         // Get the ArrayManager and save the current execution space
         chai::ArrayManager* arrayManager = chai::ArrayManager::getInstance();
         ExecutionSpace currentSpace = arrayManager->getExecutionSpace();

         // Set the execution space so that ManagedArrays and managed_ptrs
         // are handled properly
         arrayManager->setExecutionSpace(GPU);
#endif

         // Allocate space on the GPU to hold the pointer to the new object
         T** gpuBuffer;
         GPU_ERROR_CHECK(cudaMalloc(&gpuBuffer, sizeof(T*)));

         // Create the object on the device
         make_on_device<<<1, 1>>>(gpuBuffer, args...);
         debug_cudaDeviceSynchronize();

         // Allocate space on the CPU for the pointer and copy the pointer to the CPU
         T** cpuBuffer = (T**) malloc(sizeof(T*));
         GPU_ERROR_CHECK(cudaMemcpy(cpuBuffer, gpuBuffer, sizeof(T*),
                                    cudaMemcpyDeviceToHost));

         // Get the GPU pointer
         T* gpuPointer = cpuBuffer[0];

         // Free the host and device buffers
         free(cpuBuffer);
         GPU_ERROR_CHECK(cudaFree(gpuBuffer));

#ifndef CHAI_DISABLE_RM
         // Set the execution space back to the previous value
         arrayManager->setExecutionSpace(currentSpace);
#endif

         // Return the GPU pointer
         return gpuPointer;
      }

      ///
      /// @author Alan Dayton
      ///
      /// Calls a factory method to create a new object on the device.
      ///
      /// @param[in]  f    The factory method
      /// @param[in]  args The arguments to the factory method
      ///
      /// @return The device pointer to the new object
      ///
      template <typename T,
                typename F,
                typename... Args>
      CHAI_HOST T* make_on_device_from_factory(F f, Args&&... args) {
#ifndef CHAI_DISABLE_RM
         // Get the ArrayManager and save the current execution space
         chai::ArrayManager* arrayManager = chai::ArrayManager::getInstance();
         ExecutionSpace currentSpace = arrayManager->getExecutionSpace();

         // Set the execution space so that chai::ManagedArrays and
         // chai::managed_ptrs are handled properly
         arrayManager->setExecutionSpace(GPU);
#endif

         // Allocate space on the GPU to hold the pointer to the new object
         T** gpuBuffer;
         GPU_ERROR_CHECK(cudaMalloc(&gpuBuffer, sizeof(T*)));

         // Create the object on the device
         make_on_device_from_factory<T><<<1, 1>>>(gpuBuffer, f, args...);
         debug_cudaDeviceSynchronize();

         // Allocate space on the CPU for the pointer and copy the pointer to the CPU
         T** cpuBuffer = (T**) malloc(sizeof(T*));
         GPU_ERROR_CHECK(cudaMemcpy(cpuBuffer, gpuBuffer, sizeof(T*),
                                    cudaMemcpyDeviceToHost));

         // Get the GPU pointer
         T* gpuPointer = cpuBuffer[0];

         // Free the host and device buffers
         free(cpuBuffer);
         GPU_ERROR_CHECK(cudaFree(gpuBuffer));

#ifndef CHAI_DISABLE_RM
         // Set the execution space back to the previous value
         arrayManager->setExecutionSpace(currentSpace);
#endif

         // Return the GPU pointer
         return gpuPointer;
      }

#endif

      // Adapted from "The C++ Programming Language," Fourth Edition,
      // by Bjarne Stroustrup, pp. 814-816
      // Used to determine if a functor is callable with the given arguments
      struct substitution_failure {};

      template <typename T>
      struct substitution_succeeded : std::true_type {};

      template<>
      struct substitution_succeeded<substitution_failure> : std::false_type {};

      template <typename F, typename... Args>
      struct is_invocable_impl {
         private:
            template <typename X, typename... Ts>
            static auto check(X const& x, Ts&&... ts) -> decltype(x(ts...));
            static substitution_failure check(...);
         public:
            using type = decltype(check(std::declval<F>(), std::declval<Args>()...));
      };

      template <typename F, typename... Args>
      struct is_invocable : substitution_succeeded<typename is_invocable_impl<F, Args...>::type> {};
   } // namespace detail

   ///
   /// @author Alan Dayton
   ///
   /// Makes a managed_ptr<T>.
   /// Factory function to create managed_ptrs.
   ///
   /// @param[in] args The arguments to T's constructor
   ///
   template <typename T,
             typename... Args>
   CHAI_HOST managed_ptr<T> make_managed(Args... args) {
#ifdef __CUDACC__
      // Construct on the GPU first to take advantage of asynchrony
      T* gpuPointer = detail::make_on_device<T>(args...);
#endif

      // Construct on the CPU
      T* cpuPointer = detail::make_on_host<T>(args...);

      // Construct and return the managed_ptr
#ifdef __CUDACC__
      return managed_ptr<T>({CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>({CPU}, {cpuPointer});
#endif
   }

   ///
   /// @author Alan Dayton
   ///
   /// Makes a managed_ptr<T>.
   /// Factory function to create managed_ptrs.
   ///
   /// @param[in] f The factory function that will create the object
   /// @param[in] args The arguments to the factory function
   ///
   template <typename T,
             typename F,
             typename... Args>
   CHAI_HOST managed_ptr<T> make_managed_from_factory(F&& f, Args&&... args) {
      static_assert(detail::is_invocable<F, Args...>::value,
                    "F is not invocable with the given arguments.");

      static_assert(std::is_pointer<typename std::result_of<F(Args...)>::type>::value,
                    "F does not return a pointer.");

      using R = typename std::remove_pointer<typename std::result_of<F(Args...)>::type>::type;

      static_assert(std::is_convertible<R*, T*>::value,
                    "F does not return a pointer that is convertible to T*.");

#ifdef __CUDACC__
      // Construct on the GPU first to take advantage of asynchrony
      T* gpuPointer = detail::make_on_device_from_factory<R>(f, args...);
#endif

      // Construct on the CPU
      T* cpuPointer = detail::make_on_host_from_factory<R>(f, args...);

      // Construct and return the managed_ptr
#ifdef __CUDACC__
      return managed_ptr<T>({CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>({CPU}, {cpuPointer});
#endif
   }

   ///
   /// @author Alan Dayton
   ///
   /// Makes a new managed_ptr that shares ownership with the given managed_ptr, but
   ///    the underlying pointer is converted using static_cast.
   ///
   /// @param[in] other The managed_ptr to share ownership with and whose pointer to
   ///                      convert using static_cast
   ///
   template <typename T, typename U>
   CHAI_HOST managed_ptr<T> static_pointer_cast(const managed_ptr<U>& other) noexcept {
      T* cpuPointer = static_cast<T*>(other.get());

#ifdef __CUDACC__
      T* gpuPointer = static_cast<T*>(other.get(GPU, false));

      return managed_ptr<T>(other, {CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>(other, {CPU}, {cpuPointer});
#endif
   }

   ///
   /// @author Alan Dayton
   ///
   /// Makes a new managed_ptr that shares ownership with the given managed_ptr, but
   ///    the underlying pointer is converted using dynamic_cast.
   ///
   /// @param[in] other The managed_ptr to share ownership with and whose pointer to
   ///                      convert using dynamic_cast
   ///
   template <typename T, typename U>
   CHAI_HOST managed_ptr<T> dynamic_pointer_cast(const managed_ptr<U>& other) noexcept {
      T* cpuPointer = dynamic_cast<T*>(other.get());

#ifdef __CUDACC__
      T* gpuPointer = nullptr;

      if (cpuPointer) {
         gpuPointer = static_cast<T*>(other.get(GPU, false));
      }

      return managed_ptr<T>(other, {CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>(other, {CPU}, {cpuPointer});
#endif
   }

   ///
   /// @author Alan Dayton
   ///
   /// Makes a new managed_ptr that shares ownership with the given managed_ptr, but
   ///    the underlying pointer is converted using const_cast.
   ///
   /// @param[in] other The managed_ptr to share ownership with and whose pointer to
   ///                      convert using const_cast
   ///
   template <typename T, typename U>
   CHAI_HOST managed_ptr<T> const_pointer_cast(const managed_ptr<U>& other) noexcept {
      T* cpuPointer = const_cast<T*>(other.get());

#ifdef __CUDACC__
      T* gpuPointer = const_cast<T*>(other.get(GPU, false));

      return managed_ptr<T>(other, {CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>(other, {CPU}, {cpuPointer});
#endif
   }

   ///
   /// @author Alan Dayton
   ///
   /// Makes a new managed_ptr that shares ownership with the given managed_ptr, but
   ///    the underlying pointer is converted using reinterpret_cast.
   ///
   /// @param[in] other The managed_ptr to share ownership with and whose pointer to
   ///                      convert using reinterpret_cast
   ///
   template <typename T, typename U>
   CHAI_HOST managed_ptr<T> reinterpret_pointer_cast(const managed_ptr<U>& other) noexcept {
      T* cpuPointer = reinterpret_cast<T*>(other.get());

#ifdef __CUDACC__
      T* gpuPointer = reinterpret_cast<T*>(other.get(GPU, false));

      return managed_ptr<T>(other, {CPU, GPU}, {cpuPointer, gpuPointer});
#else
      return managed_ptr<T>(other, {CPU}, {cpuPointer});
#endif
   }

   /// Comparison operators

   ///
   /// @author Alan Dayton
   ///
   /// Equals comparison.
   ///
   /// @param[in] lhs The first managed_ptr to compare
   /// @param[in] rhs The second managed_ptr to compare
   ///
   template <typename T, typename U>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator==(const managed_ptr<T>& lhs, const managed_ptr<U>& rhs) noexcept {
      return lhs.get() == rhs.get();
   }

   ///
   /// @author Alan Dayton
   ///
   /// Not equals comparison.
   ///
   /// @param[in] lhs The first managed_ptr to compare
   /// @param[in] rhs The second managed_ptr to compare
   ///
   template <typename T, typename U>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator!=(const managed_ptr<T>& lhs, const managed_ptr<U>& rhs) noexcept {
      return lhs.get() != rhs.get();
   }

   /// Comparison operators with nullptr

   ///
   /// @author Alan Dayton
   ///
   /// Equals comparison with nullptr.
   ///
   /// @param[in] lhs The managed_ptr to compare to nullptr
   ///
   template <typename T>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator==(const managed_ptr<T>& lhs, std::nullptr_t) noexcept {
      return lhs.get() == nullptr;
   }

   ///
   /// @author Alan Dayton
   ///
   /// Equals comparison with nullptr.
   ///
   /// @param[in] rhs The managed_ptr to compare to nullptr
   ///
   template <typename T>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator==(std::nullptr_t, const managed_ptr<T>& rhs) noexcept {
      return nullptr == rhs.get();
   }

   ///
   /// @author Alan Dayton
   ///
   /// Not equals comparison with nullptr.
   ///
   /// @param[in] lhs The managed_ptr to compare to nullptr
   ///
   template <typename T>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator!=(const managed_ptr<T>& lhs, std::nullptr_t) noexcept {
      return lhs.get() != nullptr;
   }

   ///
   /// @author Alan Dayton
   ///
   /// Not equals comparison with nullptr.
   ///
   /// @param[in] rhs The managed_ptr to compare to nullptr
   ///
   template <typename T>
   CHAI_HOST_DEVICE CHAI_INLINE
   bool operator!=(std::nullptr_t, const managed_ptr<T>& rhs) noexcept {
      return nullptr != rhs.get();
   }

   ///
   /// @author Alan Dayton
   ///
   /// Not equals comparison.
   ///
   /// @param[in] lhs The first managed_ptr to swap
   /// @param[in] rhs The second managed_ptr to swap
   ///
   template <typename T>
   void swap(managed_ptr<T>& lhs, managed_ptr<T>& rhs) noexcept {
      std::swap(lhs.m_cpu_pointer, rhs.m_cpu_pointer);
      std::swap(lhs.m_gpu_pointer, rhs.m_gpu_pointer);
      std::swap(lhs.m_pointer_record, rhs.m_pointer_record);
   }
} // namespace chai

#endif // MANAGED_PTR

