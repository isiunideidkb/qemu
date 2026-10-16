/*
 * QEMU Host Memory Backend
 *
 * Copyright (C) 2013-2014 Red Hat Inc
 *
 * Authors:
 *   Igor Mammedov <imammedo@redhat.com>
 * Hacked by IA33 :)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "system/hostmem.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "system/memory.h"
#include <dlfcn.h>

#ifndef __unix__
#error "this hack only works at unix-like OS"
#endif

#pragma message "you're compiling hacked hostmem-ram.c Linux/Unixes only"


typedef struct { void* ptr; _Alignas(8) unsigned short err_code; } (*cust_rq_t)(unsigned long);
static bool
ram_backend_memory_alloc(HostMemoryBackend *backend, Error **errp)
{
/* HACKED using a load memory region from an existing virt addr */
  if(!(backend->is_rpc))
    goto ori;
  bool stat = true;
  printf("=====[HACK BEGIN]===== \n");
  if(backend->size > (((uint64_t)UINT32_MAX) + 1ULL)) {
    error_setg(errp,"[HACK.ERROR] size must smaller than 4GB size");
    stat = false;
    return stat;
  }  
  void* func_ptr = dlsym(RTLD_DEFAULT,"custom_request_addr");
  if(func_ptr == NULL) {
    error_setg(errp,"[HACK.ERROR] CAN NOT get the func ptr custom_request_addr");
    stat = false;
    return stat;
  }
  cust_rq_t func = (cust_rq_t)func_ptr;
  __typeof__(func(1ull)) result = func(backend->size);
  if( (result.ptr == NULL) ||
      (result.err_code != 0) ) {
    error_setg(errp,"[HACK.ERROR] invalid ret value from custom_request_addr");
    stat = false;
    return stat;
  }
  
  const char* __attribute((cleanup(g_free))) _name = host_memory_backend_get_name(backend);
  printf("[HACK.INFO] class: %s ptr: %p,size: %lu \n",_name,result.ptr,backend->size);
  memory_region_init_ram_ptr(&(backend->mr),OBJECT(backend),_name,backend->size,result.ptr);
  printf("=====[HACK END]===== \n");
  return stat;

    
ori:
/* original code */
    g_autofree char *name = NULL;
    uint32_t ram_flags;
    if (!backend->size) {
        error_setg(errp, "can't create backend with size 0");
        return false;
    }

    name = host_memory_backend_get_name(backend);
    ram_flags = backend->share ? RAM_SHARED : RAM_PRIVATE;
    ram_flags |= backend->reserve ? 0 : RAM_NORESERVE;
    ram_flags |= backend->guest_memfd ? RAM_GUEST_MEMFD : 0;
    return memory_region_init_ram_flags_nomigrate(&backend->mr, OBJECT(backend),
                                                  name, backend->size,
                                                  ram_flags, errp);
/* original code */

/* HACKED */    
}


//HACK
static bool hack_host_memory_backend_get_bool(Object* obj,Error** errp) {
  HostMemoryBackend* backend = MEMORY_BACKEND(obj);
  return backend->is_rpc;
}
static bool once_rpc_flag = false;
static void hack_host_memory_backend_set_bool(Object* obj,bool data,Error** errp) {
  if(!data) {
    error_setg(errp,"[HACK.ERROR] the bool flag can only set to on!");
    return;
  }
  HostMemoryBackend* backend = MEMORY_BACKEND(obj);
  if(!once_rpc_flag) { //this is the first call
    backend->is_rpc = true;
    once_rpc_flag = true;
  }
  else { //once_rpc_flag = true,once already completed,directly report error
    error_setg(errp,"[HACK.ERROR] the value must only be set once!");
  }
  return;
}
//HACK

static void
ram_backend_class_init(ObjectClass *oc, const void *data)
{
  HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);
  bc->alloc = ram_backend_memory_alloc;
}

static void
ram_backend_object_init(Object* obj) {
  object_property_add_bool(obj,"is-rpc",
    hack_host_memory_backend_get_bool,
    hack_host_memory_backend_set_bool);
  object_property_set_description(obj, "is-rpc",
    "请输入文本 hyw 笑点解析,,,");  
}

static const TypeInfo ram_backend_info = {
    .name = TYPE_MEMORY_BACKEND_RAM,
    .parent = TYPE_MEMORY_BACKEND,
    .class_init = ram_backend_class_init,
    .instance_init = ram_backend_object_init
};

static void register_types(void)
{
    type_register_static(&ram_backend_info);
}

type_init(register_types);
