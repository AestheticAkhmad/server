/* Copyright (c) 2015, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_session_status.cc
  Table SESSION_STATUS (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "table_session_status.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "pfs_global.h"

THR_LOCK table_session_status::m_table_lock;

PFS_engine_table_share_state
table_session_status::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_session_status::m_share=
{
  { C_STRING_WITH_LEN("session_status") },
  &pfs_readonly_world_acl,
  table_session_status::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_session_status::get_row_count,
  sizeof(pos_t),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE session_status("
  "VARIABLE_NAME VARCHAR(64) not null comment 'The session status variable name.',"
  "VARIABLE_VALUE VARCHAR(1024) comment 'The session status variable value.')") },
  true, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table*
table_session_status::create(void)
{
  return new table_session_status();
}

ha_rows table_session_status::get_row_count(void)
{
  mysql_mutex_lock(&LOCK_status);
  ha_rows status_var_count= all_status_vars.elements;
  mysql_mutex_unlock(&LOCK_status);
  return status_var_count;
}

table_session_status::table_session_status()
  : PFS_engine_table(&m_share, &m_pos),
    m_status_cache(false), m_row_exists(false), m_pos(0), m_next_pos(0)
{}

void table_session_status::reset_position(void)
{
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

int table_session_status::rnd_init(bool scan)
{
 /* Build a cache of all status variables for this thread. */
  m_status_cache.materialize_all(current_thd);

  /* Record the current number of status variables to detect subsequent changes. */
  ulonglong status_version= m_status_cache.get_status_array_version();

  /*
    The table context holds the current version of the global status array.
    If scan == true, then allocate a new context from mem_root and store in TLS.
    If scan == false, then restore from TLS.
  */
  m_context= current_thd->alloc<table_session_status_context>(1);
  new (m_context) table_session_status_context(status_version, !scan);
  return 0;
}

int table_session_status::rnd_next(void)
{
  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < m_status_cache.size();
       m_pos.next())
  {
    if (m_status_cache.is_materialized())
    {
      const Status_variable *stat_var= m_status_cache.get(m_pos.m_index);
      if (stat_var != NULL)
      {
        make_row(stat_var);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }
  return HA_ERR_END_OF_FILE;
}

int
table_session_status::rnd_pos(const void *pos)
{
  /* If global status array has changed, do nothing. */ // TODO: warning
  if (!m_context->versions_match())
    return HA_ERR_RECORD_DELETED;

  set_position(pos);
  assert(m_pos.m_index < m_status_cache.size());

  if (m_status_cache.is_materialized())
  {
    const Status_variable *stat_var= m_status_cache.get(m_pos.m_index);
    if (stat_var != NULL)
    {
      make_row(stat_var);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_session_status
::make_row(const Status_variable *status_var)
{
  m_row_exists= false;
  m_row.m_variable_name.make_row(status_var->m_name, status_var->m_name_length);
  m_row.m_variable_value.make_row(status_var);
  m_row_exists= true;
}

int table_session_status
::read_row_values(TABLE *table,
                  unsigned char *buf,
                  Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(!m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* VARIABLE_NAME */
        set_field_varchar_utf8(f, m_row.m_variable_name.m_str, m_row.m_variable_name.m_length);
        break;
      case 1: /* VARIABLE_VALUE */
        m_row.m_variable_value.set_field(f);
        break;
      default:
        assert(false);
      }
    }
  }

  return 0;
}

