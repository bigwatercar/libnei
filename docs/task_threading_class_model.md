# neixx Task & Threading 模块类图

> 生成日期：2026-08-07
> 范围：`src/neixx/task` 与 `src/neixx/threading` 的公共 API 及关键内部协作类。
> 约定：类名统一使用反引号 `` ` `` 包裹（兼容 Mermaid 解析，尤其含 `::`、`<T>` 的类型）。

---

## 1. 模块总览

```mermaid
classDiagram
    direction LR

    subgraph threading [threading 模块]
        class `nei::Thread`
        class `nei::SimpleThread`
        class `nei::PlatformThread`
        class `nei::ThreadType`
        class `nei::ThreadLocal`
        class `nei::ThreadLocalStorage`
    end

    subgraph task [task 模块]
        class `nei::SequenceManager`
        class `nei::RunLoop`
        class `nei::MessagePump`
        class `nei::ThreadPool`
        class `nei::TaskRunner`
    end

    `nei::Thread` --> `nei::SequenceManager` : 拥有(经 TaskRunner)
    `nei::Thread` --> `nei::TaskRunner` : 持有
    `nei::RunLoop` --> `nei::SequenceManager` : 驱动
    `nei::SequenceManager` --> `nei::MessagePump` : 持有
    `nei::ThreadPool` --> `nei::TaskRunner` : 创建
```

---

## 2. 线程核心：PlatformThread / Delegate / Handle / ThreadType

```mermaid
classDiagram
    class `nei::PlatformThread` {
        <<final 静态门面>>
        +static CurrentId() PlatformThreadId
        +static YieldCurrentThread()
        +static Sleep(duration)
        +static Create(size, delegate, handle) bool
        +static CreateWithType(size, delegate, handle, type) bool
        +static Join(handle) bool
        +static Detach(handle) bool
        +static SetCurrentThreadName(name)
        +static SetCurrentThreadType(type) bool
    }

    class `nei::PlatformThread::Delegate` {
        <<interface>>
        +virtual ThreadMain() void
    }

    class `nei::PlatformThread::Handle` {
        <<final 值语义令牌>>
        -unique_ptr~Impl~ impl_
        +operator bool()
    }

    class `nei::PlatformThread::Handle::Impl` {
        <<PIMPL>>
        -native_handle  HANDLE/pthread_t
        -joinable bool
        -thread_type ThreadType
    }

    class `nei::ThreadType` {
        <<enum class>>
        BACKGROUND
        DEFAULT
        REALTIME_AUDIO
    }

    `nei::PlatformThread` ..> `nei::PlatformThread::Delegate` : 创建时使用
    `nei::PlatformThread` ..> `nei::PlatformThread::Handle` : Create填充/Join/Detach
    `nei::PlatformThread` ..> `nei::ThreadType` : 使用(参数)
    `nei::PlatformThread::Handle` *-- `nei::PlatformThread::Handle::Impl` : 组合
    `nei::PlatformThread::Handle::Impl` --> `nei::ThreadType` : 持有
```

---

## 3. 托管线程：Thread / SimpleThread

```mermaid
classDiagram
    class `nei::Thread` {
        <<final>>
        +Start() bool
        +StartWithOptions(options) bool
        +Stop()
        +GetTaskRunner() scoped_refptr~SingleThreadTaskRunner~
        +IsRunning() bool
        +GetThreadId() PlatformThreadId
        -name_ string
        -handle_ PlatformThread::Handle
        -task_runner_ scoped_refptr~SingleThreadTaskRunner~
    }

    class `nei::Thread::Options` {
        message_pump_type MessagePumpType
        stack_size size_t
        thread_type ThreadType
    }

    class `nei::SimpleThread` {
        +Start()
        +Join()
        +ThreadMain()  [override Run()]
    }

    class `nei::SimpleThread::Options` {
    }

    `nei::Thread` ..|> `nei::PlatformThread::Delegate` : 实现
    `nei::SimpleThread` ..|> `nei::PlatformThread::Delegate` : 实现
    `nei::Thread` *-- `nei::PlatformThread::Handle` : 组合
    `nei::Thread` *-- `nei::Thread::Options` : 组合
    `nei::Thread` --> `nei::SingleThreadTaskRunner` : 持有
    `nei::Thread` --> `nei::MessagePumpType` : 使用
    `nei::Thread` --> `nei::ThreadType` : 使用
```

---

## 4. 消息泵体系

```mermaid
classDiagram
    class `nei::MessagePump` {
        <<abstract>>
        +virtual Run(delegate)
        +virtual Quit()
        +virtual ScheduleWork()
        +virtual ScheduleDelayedWork(deadline)
        +virtual ScheduleWorkAndDelayedWork(deadline)
    }

    class `nei::MessagePump::Delegate` {
        <<interface>>
        +virtual DoWork() bool
        +virtual DoDelayedWork(info) bool
        +virtual DoIdleWork() bool
        +NextWorkInfo
    }

    class `nei::MessagePump::Delegate::NextWorkInfo` {
        next_run_time TimeTicks
        recent_now TimeTicks
    }

    class `nei::MessagePumpDefault` {
        <<final>>
        -unique_ptr~Impl~ impl_
    }

    class `nei::MessagePumpForIO` {
        <<final>>
        +RegisterWatch()
        -MessagePumpForIOState state
    }

    class `nei::MessagePumpForIO::Watcher` {
        <<interface>>
        +OnFileCanReadWithoutBlocking(fd)
        +OnFileCanWriteWithoutBlocking(fd)
    }

    class `nei::MessagePumpForIO::CompletionWatcher` {
        +OnIOCompleted()
    }

    class `nei::MessagePumpForIO::FdWatchController` {
        <<final>>
        +StartWatching(fd, mode, watcher)
        +StopWatching()
    }

    class `nei::MessagePumpType` {
        <<enum class>>
        DEFAULT
        IO
    }

    `nei::MessagePumpDefault` --|> `nei::MessagePump` : 继承
    `nei::MessagePumpForIO` --|> `nei::MessagePump` : 继承
    `nei::MessagePumpForIO` *-- `nei::MessagePumpForIO::FdWatchController` : 管理
    `nei::MessagePumpForIO::CompletionWatcher` --|> `nei::MessagePumpForIO::Watcher` : 继承
    `nei::SequenceManager` ..|> `nei::MessagePump::Delegate` : 实现
    `nei::Thread` --> `nei::MessagePumpType` : 选择
```

---

## 5. 序列与任务循环：SequenceManager / RunLoop

```mermaid
classDiagram
    class `nei::SequenceManager` {
        <<final>>
        +static Current() SequenceManager*
        +CreateTaskRunner(traits) scoped_refptr~SequencedTaskRunner~
        +GetDefaultTaskRunner() scoped_refptr~SingleThreadTaskRunner~
        -unique_ptr~Impl~ impl_
    }

    class `nei::RunLoop` {
        <<final>>
        +Run()
        +Quit()
        -sequence_manager_ SequenceManager*
    }

    class `nei::ThreadTaskRunnerHandle` {
        <<final>>
        +static Get() scoped_refptr~SingleThreadTaskRunner~
    }

    class `nei::SequenceToken` {
        +Create() SequenceToken
    }

    `nei::SequenceManager` ..|> `nei::MessagePump::Delegate` : 实现
    `nei::SequenceManager` *-- `nei::MessagePump` : 持有(unique_ptr)
    `nei::SequenceManager` --> `nei::SequencedTaskRunner` : 创建
    `nei::SequenceManager` --> `nei::SingleThreadTaskRunner` : 创建默认
    `nei::RunLoop` --> `nei::SequenceManager` : 驱动
    `nei::ThreadTaskRunnerHandle` --> `nei::SequenceManager` : 查当前
    `nei::SequenceManager` --> `nei::SequenceToken` : 关联
```

---

## 6. TaskRunner 继承体系

```mermaid
classDiagram
    class `nei::TaskRunner` {
        <<abstract RefCountedThreadSafe>>
        +PostTask(closure) bool
        +PostDelayedTask(closure, delay) bool
    }

    class `nei::SequencedTaskRunner` {
        +RunsTasksInCurrentSequence() bool
        -Impl impl_
    }

    class `nei::SingleThreadTaskRunner` {
        +BelongsToCurrentThread() bool
    }

    `nei::SequencedTaskRunner` --|> `nei::TaskRunner` : 继承
    `nei::SingleThreadTaskRunner` --|> `nei::SequencedTaskRunner` : 继承
```

---

## 7. 线程池

```mermaid
classDiagram
    class `nei::ThreadPool` {
        <<final>>
        +CreateSequencedTaskRunner(traits) scoped_refptr~SequencedTaskRunner~
        +CreateSingleThreadTaskRunner(traits) scoped_refptr~SingleThreadTaskRunner~
        +CreateParallelTaskRunner(traits) scoped_refptr~TaskRunner~
        -unique_ptr~Impl~ impl_
    }

    class `nei::ThreadPool::InitParams` {
        workers int
        max_workers int
    }

    class `nei::ThreadPoolInstance` {
        <<final 全局单例>>
        +Get() ThreadPoolInstance*
        +CreateSequencedTaskRunner(traits)
        +CreateSingleThreadTaskRunner(traits)
        +CreateParallelTaskRunner(traits)
        +ResetForTesting()
    }

    `nei::ThreadPool` --> `nei::SequencedTaskRunner` : 创建
    `nei::ThreadPool` --> `nei::SingleThreadTaskRunner` : 创建
    `nei::ThreadPool` --> `nei::TaskRunner` : 创建(并行)
    `nei::ThreadPool` *-- `nei::ThreadPool::InitParams` : 组合
    `nei::ThreadPoolInstance` ..> `nei::ThreadPool` : 委托(门面)
    `nei::ThreadPoolInstance` ..> `nei::TaskRunner` : 创建
```

---

## 8. 定时器与 Job

```mermaid
classDiagram
    class `nei::OneShotTimer` {
        <<final>>
        +Start(delay, closure)
        +Stop()
        -unique_ptr~Impl~ impl_
    }

    class `nei::RepeatingTimer` {
        <<final>>
        +Start(interval, closure)
        +Stop()
        -unique_ptr~Impl~ impl_
    }

    class `nei::JobHandle` {
        +Join()
        -unique_ptr~Impl~ impl_
    }

    class `nei::JobDelegate` {
        +ShouldYield() bool
        +GetMaxConcurrency() size_t
    }

    `nei::OneShotTimer` --> `nei::SequencedTaskRunner` : 持有
    `nei::RepeatingTimer` --> `nei::SequencedTaskRunner` : 持有
    `nei::JobHandle` --> `nei::TaskRunner` : 关联(并行投递)
    `nei::JobDelegate` <-- `nei::JobHandle` : 协作
```

---

## 9. 线程本地存储（ThreadLocal 体系）

```mermaid
classDiagram
    class `nei::ThreadLocalSlot` {
        <<内部基座>>
    }

    class `nei::ThreadLocal~T~` {
        +Get() T
        +Set(value)
    }

    class `nei::ThreadLocalPointer~T~` {
        +Get() T*
        +Set(ptr)
    }

    class `nei::ThreadLocalOwnedPointer~T~` {
        +Get() T*
        +Set(unique_ptr~T~)
    }

    class `nei::ThreadLocalBoolean` {
        +Get() bool
        +Set(value)
    }

    class `nei::ThreadLocalStorage` {
        <<命名空间>>
        +Slot   [deprecated]
        +Iterator
    }

    `nei::ThreadLocal~T~` --> `nei::ThreadLocalSlot` : 底层
    `nei::ThreadLocalPointer~T~` --> `nei::ThreadLocalSlot` : 底层
    `nei::ThreadLocalOwnedPointer~T~` --> `nei::ThreadLocalSlot` : 底层
    `nei::ThreadLocalBoolean` --> `nei::ThreadLocalSlot` : 底层
    `nei::ThreadLocalStorage::Slot` ..> `nei::ThreadLocalPointer~T~` : 迁移目标
```

---

## 10. 诊断与辅助

```mermaid
classDiagram
    class `nei::SequenceChecker` {
        +CalledOnValidSequence() bool
    }

    class `nei::ThreadChecker` {
        +CalledOnValidThread() bool
    }

    class `nei::TaskObserver` {
        +WillProcessTask(observed)
        +DidProcessTask(observed)
    }

    class `nei::ObservedTask` {
        +posted_from Location
    }

    class `nei::TaskTraits` {
        +priority() TaskPriority
        +shutdown_behavior() TaskShutdownBehavior
    }

    class `nei::TaskPriority` {
        <<enum class>>
        BEST_EFFORT
        USER_VISIBLE
        USER_BLOCKING
    }

    class `nei::TaskShutdownBehavior` {
        <<enum class>>
        CONTINUE_ON_SHUTDOWN
        SKIP_ON_SHUTDOWN
        BLOCK_SHUTDOWN
    }

    class `nei::ScopedBlockingCall` {
        <<final RAII>>
    }

    `nei::ThreadChecker` ..> `nei::ThreadLocal~bool~` : 底层
    `nei::TaskObserver` --> `nei::ObservedTask` : 入参
    `nei::TaskTraits` --> `nei::TaskPriority` : 持有
    `nei::TaskTraits` --> `nei::TaskShutdownBehavior` : 持有
```

---

## 附：关键内部协作类（`nei::internal`）

```mermaid
classDiagram
    class `nei::internal::TaskQueue` {
        +PushImmediateTask(task)
        +TakeImmediateTask(task)
        +SetOnTaskPostedCallback(cb)
    }

    class `nei::internal::Task` {
        +callback OnceClosure
        +delayed_run_time TimeTicks
        +sequence_num int64
    }

    class `nei::internal::StartState` {
        delegate PlatformThread::Delegate*
        thread_type ThreadType
    }

    `nei::TaskRunner` ..> `nei::internal::TaskQueue` : 投递
    `nei::internal::TaskQueue` o-- `nei::internal::Task` : 队列元素
    `nei::PlatformThread` ..> `nei::internal::StartState` : 线程启动转发
```
