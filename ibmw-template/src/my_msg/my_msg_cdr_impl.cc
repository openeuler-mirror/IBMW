// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Demo 8 — single-TU CDR wrapper for MyMsg.
//
// Compile this in EXACTLY ONE translation unit per link target. The static
// library produced by `ibmw_add_dds_messages(my_msg_cdr ... CDR_IMPLS ...)`
// owns this object so all downstream modules can simply link the named
// target — no per-module duplication required.

#include "MyMsgCdrAux.ipp"
