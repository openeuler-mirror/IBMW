# LoanedMessage Demo Architecture Design

## Module Architecture

```
┌─────────────────────────────────────────┐
│         Application (main.cc)           │
│  SetDdsBackend() injects Backend ptr    │
└─────────────────┬───────────────────────┘
                  │
    ┌─────────────┼─────────────┐
    ↓             ↓             ↓
┌─────────┐ ┌──────────┐ ┌───────────┐
│ Plain   │ │ NonPlain │ │ Config    │
│ Pub     │ │ Pub      │ │ Test      │
│ Module  │ │ Module   │ │ Module    │
└────┬────┘ └────┬─────┘ └─────┬─────┘
     │           │             │
     ↓           ↓             ↓
┌─────────────────────────────────────────┐
│      Ros2StylePublisher<MsgType>        │
│  borrow_loaned_message() / publish()    │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────┴───────────────────────┐
│       LoanedMessage<MsgType>            │
│  is_valid / is_zero_copy / is_plain     │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────┴───────────────────────┐
│       DdsTransportBackend                 │
│  GetDataWriter / IsPlainType / Stats    │
└─────────────────┬───────────────────────┘
                  │
┌─────────────────┴───────────────────────┐
│    FastDDS DataWriter::loan_sample()    │
└─────────────────────────────────────────┘
```

## Backend Acquisition

Via global singleton (global.h/cc):
- `dds_extension.cc` calls `SetDdsTransportBackend(ptr)` during `RegisterDdsTransportBackend()`
- The app's `main.cc` calls `GetDdsTransportBackend()` after `core.Initialize()` to obtain the pointer
- Injects into modules via `module.SetDdsBackend(backend)`

## Message Flow

Plain type (zero-copy):
```
borrow_loaned_message() → loan_sample() → shared memory pointer
  → user fills data → publish(std::move()) → write(void*) → zero-copy transfer
```

Non-Plain type (fallback):
```
borrow_loaned_message() → is_plain=false → new MsgType() (heap memory)
  → verify is_zero_copy=false → discard loaned message
  → traditional Publish<T>() → serialize → DDS transfer
```

## Configuration Modes

- AUTO: Automatically attempts zero-copy; falls back to heap memory on failure
- STRICT: Forces zero-copy; returns invalid object for non-Plain types
- COMPAT: Disables zero-copy; forces heap memory

## Key Design Decisions

1. Non-Plain types cannot be published via `DataWriter::write(void*)`; the traditional API must be used
2. Template header files (loaned_message_impl.h) cannot use plugin log macros (GetLogger scope issue)
3. In ROS2 compatibility mode, topic names require `rt/` prefix conversion
4. `write(void*)` returns `bool`; `loan_sample()` returns `ReturnCode_t`
