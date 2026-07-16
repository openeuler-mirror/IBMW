// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Demo 8 — DDS type bridge for MyMsg. Include from any module that
// publishes or subscribes my_project::my_msg::MyMsg.

#pragma once

#include "internal/typesupport/dds/dds_transport.h"

#include "MyMsg.hpp"
#include "MyMsgCdrAux.ipp"

IBMW_DDS_TYPE_NAME(my_project::my_msg::MyMsg, "my_project::my_msg::MyMsg")
