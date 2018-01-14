/*
 * Copyright (c) 2018 Flössie <floessie.mail@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <Arduino.h>
#include <Arduino_FreeRTOS.h>
#include <queue.h>
#include <semphr.h>

namespace frt
{

	namespace detail {

		inline void yieldFromIsr() __attribute__((always_inline));

		void yieldFromIsr()
		{
			#ifdef portYIELD_FROM_ISR
			portYIELD_FROM_ISR();
			#else
				#ifdef portEND_SWITCHING_ISR
					portEND_SWITCHING_ISR();
				#else
					taskYIELD();
				#endif
			#endif
		}

	}

	template<typename T, unsigned int STACK_SIZE = configMINIMAL_STACK_SIZE * sizeof(StackType_t)>
	class Task
	{
	public:
		Task() :
			running(false),
			do_stop(false),
			handle(nullptr)
		{
		}

		~Task()
		{
			stop();
		}

		explicit Task(const Task& other) = delete;
		Task& operator =(const Task& other) = delete;

		bool start(unsigned char priority = 0, const char* name = "")
		{
			if (priority >= configMAX_PRIORITIES) {
				priority = configMAX_PRIORITIES - 1;
			}

#if configSUPPORT_STATIC_ALLOCATION > 0
			handle = xTaskCreateStatic(
				entryPoint,
				name,
				STACK_SIZE / sizeof(StackType_t),
				this,
				priority,
				stack,
				&state
			);
			return handle;
#else
			return
				xTaskCreate(
					entryPoint,
					name,
					STACK_SIZE / sizeof(StackType_t),
					this,
					priority,
					&handle
				) == pdPASS;
#endif
		}

		bool stop()
		{
			if (!handle) {
				return false;
			}

			do_stop = true;
			while (running) {
				yield();
			}

			return true;
		}

		bool isRunning() const
		{
			return running;
		}

		unsigned int getUsedStackSize() const
		{
			return STACK_SIZE - uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t);
		}

		void post()
		{
			xTaskNotifyGive(handle);
		}

		void preparePostFromInterrupt()
		{
			higher_priority_task_woken = 0;
		}

		void postFromInterrupt()
		{
			vTaskNotifyGiveFromISR(handle, &higher_priority_task_woken);
		}

		void finalizePostFromInterrupt() __attribute__((always_inline))
		{
			if (higher_priority_task_woken) {
				detail::yieldFromIsr();
			}
		}

	protected:
		void yield()
		{
			taskYIELD();
		}

		void msleep(unsigned int msecs)
		{
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;

			if (ticks) {
				vTaskDelay(ticks);
			} else {
				yield();
			}
		}

		void msleep(unsigned int msecs, unsigned int& remainder)
		{
			msecs += remainder;
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;
			remainder = msecs % portTICK_PERIOD_MS;

			if (ticks) {
				vTaskDelay(ticks);
			} else {
				yield();
			}
		}

		void wait()
		{
			ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
		}

		bool wait(unsigned int msecs)
		{
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;

			return ulTaskNotifyTake(pdFALSE, ticks);
		}

		bool wait(unsigned int msecs, unsigned int& remainder)
		{
			msecs += remainder;
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;
			remainder = msecs % portTICK_PERIOD_MS;

			if (ulTaskNotifyTake(pdFALSE, ticks)) {
				remainder = 0;
				return true;
			}

			return false;
		}

	private:
		static void entryPoint(void* data)
		{
			Task* const self = static_cast<Task*>(data);

			self->running = true;
			while (static_cast<T*>(self)->run() && !self->do_stop);
			self->do_stop = false;
			self->running = false;

			const TaskHandle_t handle_copy = self->handle;
			self->handle = nullptr;

			vTaskDelete(handle_copy);
		}

		volatile bool running;
		volatile bool do_stop;
		TaskHandle_t handle;
		BaseType_t higher_priority_task_woken;
#ifdef configSUPPORT_STATIC_ALLOCATION
		StackType_t stack[STACK_SIZE / sizeof(StackType_t)];
		StaticTask_t state;
#endif
	};

	template<typename T, int ITEMS>
	class Queue
	{
	public:
		Queue() :
			handle(
#ifdef configSUPPORT_STATIC_ALLOCATION
				xQueueCreateStatic(ITEMS, sizeof(T), buffer, &state)
#else
				xQueueCreate(ITEMS, sizeof(T))
#endif
			)
		{
		}

		~Queue()
		{
			vQueueDelete(handle);
		}

		explicit Queue(const Queue& other) = delete;
		Queue& operator =(const Queue& other) = delete;

		void push(const T& item)
		{
			xQueueSend(handle, &item, portMAX_DELAY);
		}

		bool push(const T& item, unsigned int msecs)
		{
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;

			return xQueueSend(handle, &item, ticks) == pdTRUE;
		}

		bool push(const T& item, unsigned int msecs, unsigned int& remainder)
		{
			msecs += remainder;
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;
			remainder = msecs % portTICK_PERIOD_MS;

			if (xQueueSend(handle, &item, ticks) == pdTRUE) {
				remainder = 0;
				return true;
			}

			return false;
		}

		void preparePushFromInterrupt()
		{
			higher_priority_task_woken_from_push = 0;
		}

		bool pushFromInterrupt(const T& item)
		{
			return xQueueSendFromISR(handle, &item, &higher_priority_task_woken_from_push) == pdTRUE;
		}

		void finalizePushFromInterrupt() __attribute__((always_inline))
		{
			if (higher_priority_task_woken_from_push) {
				detail::yieldFromIsr();
			}
		}

		void pop(T& item)
		{
			xQueueReceive(handle, &item, portMAX_DELAY);
		}

		bool pop(unsigned int msecs, T& item)
		{
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;

			return xQueueReceive(handle, &item, ticks) == pdTRUE;
		}

		bool pop(unsigned int msecs, unsigned int& remainder, T& item)
		{
			msecs += remainder;
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;
			remainder = msecs % portTICK_PERIOD_MS;

			if (xQueueReceive(handle, &item, ticks) == pdTRUE) {
				remainder = 0;
				return true;
			}

			return false;
		}

		void preparePopFromInterrupt()
		{
			higher_priority_task_woken_from_pop = 0;
		}

		bool popFromInterrupt(const T& item)
		{
			return xQueueReceiveFromISR(handle, &item, &higher_priority_task_woken_from_pop);
		}

		void finalizePopFromInterrupt() __attribute__((always_inline))
		{
			if (higher_priority_task_woken_from_pop) {
				detail::yieldFromIsr();
			}
		}

	private:
		QueueHandle_t handle;
		BaseType_t higher_priority_task_woken_from_push;
		BaseType_t higher_priority_task_woken_from_pop;
#ifdef configSUPPORT_STATIC_ALLOCATION
		uint8_t buffer[ITEMS * sizeof(T)];
		StaticQueue_t state;
#endif
	};

	class Mutex
	{
	public:
		Mutex() :
			handle(
#if configSUPPORT_STATIC_ALLOCATION > 0
				xSemaphoreCreateMutexStatic(&buffer)
#else
				xSemaphoreCreateMutex()
#endif
			)
		{
		}

		~Mutex()
		{
			vSemaphoreDelete(handle);
		}

		explicit Mutex(const Mutex& other) = delete;
		Mutex& operator =(const Mutex& other) = delete;

		void lock()
		{
			xSemaphoreTake(handle, portMAX_DELAY);
		}

		void unlock()
		{
			xSemaphoreGive(handle);
		}

	private:
		SemaphoreHandle_t handle;
#ifdef configSUPPORT_STATIC_ALLOCATION
		StaticSemaphore_t buffer;
#endif
	};

	class Semaphore
	{
	public:
		Semaphore(bool binary = false) :
			handle(
#if configSUPPORT_STATIC_ALLOCATION > 0
				[this, binary]()
				{
					return
						binary
							? xSemaphoreCreateBinaryStatic(&buffer)
							: xSemaphoreCreateCountingStatic(static_cast<UBaseType_t>(-1), 0, &buffer);
				}()
#else
				[binary]()
				{
					return
						binary
							? xSemaphoreCreateBinary()
							: xSemaphoreCreateCounting(static_cast<UBaseType_t>(-1), 0);
				}()
#endif
			)
		{
		}

		~Semaphore()
		{
			vSemaphoreDelete(handle);
		}

		explicit Semaphore(const Semaphore& other) = delete;
		Semaphore& operator =(const Semaphore& other) = delete;

		void wait()
		{
			xSemaphoreTake(handle, portMAX_DELAY);
		}

		bool wait(unsigned int msecs)
		{
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;

			return xSemaphoreTake(handle, ticks) == pdTRUE;
		}

		bool wait(unsigned int msecs, unsigned int& remainder)
		{
			msecs += remainder;
			const TickType_t ticks = msecs / portTICK_PERIOD_MS;
			remainder = msecs % portTICK_PERIOD_MS;

			if (xSemaphoreTake(handle, ticks) == pdTRUE) {
				remainder = 0;
				return true;
			}

			return false;
		}

		void post()
		{
			xSemaphoreGive(handle);
		}

		void preparePostFromInterrupt()
		{
			higher_priority_task_woken = 0;
		}

		void postFromInterrupt()
		{
			xSemaphoreGiveFromISR(handle, &higher_priority_task_woken);
		}

		void finalizePostFromInterrupt() __attribute__((always_inline))
		{
			if (higher_priority_task_woken) {
				detail::yieldFromIsr();
			}
		}

	private:
		SemaphoreHandle_t handle;
		BaseType_t higher_priority_task_woken;
#ifdef configSUPPORT_STATIC_ALLOCATION
		StaticSemaphore_t buffer;
#endif
	};

}