/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include <glib-object.h>

#ifdef G_DECLARE_DERIVABLE_TYPE
#  define GARROW_DECLARE_TYPE(ObjectName,       \
                              object_name,      \
                              MODULE_NAME,      \
                              OBJECT_NAME,      \
                              ParentName)       \
  G_DECLARE_DERIVABLE_TYPE(ObjectName,          \
                           object_name,         \
                           MODULE_NAME,         \
                           OBJECT_NAME,         \
                           ParentName)          \
  struct _ ## ObjectName ## Class               \
  {                                             \
    ParentName ## Class parent_class;           \
  };
#else
#  define GARROW_DECLARE_TYPE(ObjectName,                               \
                              object_name,                              \
                              MODULE_NAME,                              \
                              OBJECT_NAME,                              \
                              ParentName)                               \
  typedef struct _ ## ObjectName ObjectName;                            \
  typedef struct _ ## ObjectName ## Class ObjectName ## Class;          \
                                                                        \
  struct _ ## ObjectName                                                \
  {                                                                     \
    ParentName parent_instance;                                         \
  };                                                                    \
                                                                        \
  struct _ ## ObjectName ## Class                                       \
  {                                                                     \
    ParentName ## Class parent_class;                                   \
  };                                                                    \
                                                                        \
  GType object_name ## _get_type(void) G_GNUC_CONST;                    \
                                                                        \
  static inline ObjectName *                                            \
  MODULE_NAME ## _ ## OBJECT_NAME(gpointer object)                      \
  {                                                                     \
    return G_TYPE_CHECK_INSTANCE_CAST(object,                           \
                                      object_name ## _get_type(),       \
                                      ObjectName);                      \
  }                                                                     \
                                                                        \
  static inline ObjectName ## Class *                                   \
  MODULE_NAME ## _ ## OBJECT_NAME ## _CLASS(gpointer klass)             \
  {                                                                     \
    return G_TYPE_CHECK_CLASS_CAST(klass,                               \
                                   object_name ## _get_type(),          \
                                   ObjectName ## Class);                \
  }                                                                     \
                                                                        \
  static inline gboolean                                                \
  MODULE_NAME ## _IS_ ## OBJECT_NAME(gpointer object)                   \
  {                                                                     \
    return G_TYPE_CHECK_INSTANCE_TYPE(object,                           \
                                      object_name ## _get_type());      \
  }                                                                     \
                                                                        \
  static inline gboolean                                                \
  MODULE_NAME ## _IS_ ## OBJECT_NAME ## _CLASS(gpointer klass)          \
  {                                                                     \
    return G_TYPE_CHECK_CLASS_TYPE(klass,                               \
                                   object_name ## _get_type());         \
  }                                                                     \
                                                                        \
  static inline ObjectName ## Class *                                   \
  MODULE_NAME ## _ ## ObjectName ## _GET_CLASS(gpointer object)         \
  {                                                                     \
    return G_TYPE_INSTANCE_GET_CLASS(object,                            \
                                     object_name ## _get_type(),        \
                                     ObjectName ## Class);              \
  }
#endif