#pragma once

#include <ATen/core/Generator.h>
#include <torch/csrc/utils/pycfunction_helpers.h>

// TO DO: remove this file after PR to stock-PyTorch
PyObject* THPGenerator_New(PyObject* _self, PyObject* args, PyObject* kwargs);
