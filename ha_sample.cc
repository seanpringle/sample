/* Copyright (c) 2014 Sean Pringle sean.pringle@gmail.com

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file ha_sample.cc
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#include <mysql/plugin.h>
#include "ha_sample.h"
#include "sql_class.h"
#include <pthread.h>
#include <zlib.h>
#include <time.h>

static uint sample_verbose;
static uint sample_rate;
static uint sample_limit;

static list_t *sample_tables;
static pthread_mutex_t sample_tables_mutex;

static uint64 sample_seed;
static pthread_mutex_t sample_seed_mutex;

ulonglong sample_counter_rows_inserted;
static pthread_mutex_t sample_stats_mutex;

static handler *sample_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);

handlerton *sample_hton;

struct ha_table_option_struct{};
struct ha_field_option_struct{};
ha_create_table_option sample_table_option_list[] = { HA_TOPTION_END };
ha_create_table_option sample_field_option_list[] = { HA_FOPTION_END };

static void sample_note(const char *format, ...)
{
  char buff[1024];
  snprintf(buff, sizeof(buff), "SAMPLE: %s", format);
  va_list args;
  va_start(args, format);
  error_log_print(INFORMATION_LEVEL, buff, args);
  va_end(args);
}

static void sample_error(const char *format, ...)
{
  char buff[1024];
  snprintf(buff, sizeof(buff), "SAMPLE: %s", format);
  va_list args;
  va_start(args, format);
  error_log_print(ERROR_LEVEL, buff, args);
  va_end(args);
}

#define sample_assert(f,...) do { if (!(f)) { sample_error(__VA_ARGS__); abort(); } } while(0)
#define sample_debug(...) if (sample_verbose) sample_note(__VA_ARGS__)

static void* sample_alloc(size_t bytes)
{
  void *ptr = calloc(bytes, 1);
  sample_assert(ptr, "calloc failed %llu bytes", bytes);
  return ptr;
}

static void* sample_realloc(void *ptr, size_t bytes)
{
  void *ptr2 = realloc(ptr, bytes);
  sample_assert(ptr2, "realloc failed %llu bytes", bytes);
  return ptr2;
}

static void sample_free(void *ptr)
{
  free(ptr);
}

static str_t* str_alloc(size_t limit)
{
  str_t *str = (str_t*) sample_alloc(sizeof(str_t));
  str->buffer = (char*) sample_alloc(limit);
  str->limit  = limit;
  str->length = 0;
  return str;
}

static void str_free(str_t *str)
{
  sample_free(str->buffer);
  sample_free(str);
}

static void str_reset(str_t *str)
{
  str->length = 0;
}

static void str_cat(str_t *str, char *buffer, size_t length)
{
  if (buffer && length > 0)
  {
    if (str->length + length + 1 > str->limit)
    {
      str->limit = str->length + length + 1;
      str->buffer = (char*) sample_realloc(str->buffer, str->limit);
    }
    memmove(str->buffer + str->length, buffer, length);
    str->length += length;
    str->buffer[str->length] = 0;
  }
}

static void str_print(str_t *str, const char *format, ...)
{
  char buff[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(buff, sizeof(buff), format, args);
  va_end(args);
  if (length >= 0)
    str_cat(str, buff, length);
}

static list_t* list_alloc()
{
  list_t *list = (list_t*) sample_alloc(sizeof(list_t));
  return list;
}

static void list_free(list_t *list)
{
  sample_free(list);
}

static bool list_is_empty(list_t *list)
{
  return list->head ? FALSE: TRUE;
}

static void list_insert_head(list_t *list, void *item)
{
  node_t *node = (node_t*) sample_alloc(sizeof(node_t));
  node->payload = item;
  node->next = list->head;
  list->head = node;
  list->length++;
}

static void* list_remove_node(list_t *list, node_t *node)
{
  node_t **prev = &list->head;
  while (prev && *prev != node)
    prev = &(*prev)->next;

  void *payload = node->payload;

  *prev = node->next;
  list->length--;
  sample_free(node);

  return payload;
}

static void* list_remove_head(list_t *list)
{
  return (list->head) ? list_remove_node(list, list->head): NULL;
}
/*
static node_t* list_locate(list_t *list, void *item)
{
  node_t *node = list->head;
  while (node && item != node->payload)
    node = node->next;
  return node;
}
*/
static bool list_delete(list_t *list, void *item)
{
  node_t **prev = &list->head;
  while (prev && (*prev) && (*prev)->payload != item)
    prev = &(*prev)->next;

  if (prev && *prev)
  {
    node_t *node = *prev;
    *prev = node->next;
    list->length--;
    sample_free(node);
    return TRUE;
  }
  return FALSE;
}

static SampleTable* sample_table_open(const char *name, uint width, uint rate, uint limit)
{
  node_t *node = sample_tables->head;
  while (node && strcmp(((SampleTable*)node->payload)->name, name) != 0)
    node = node->next;

  SampleTable *table = node ? (SampleTable*) node->payload: NULL;

  if (!table && width && rate)
  {
    table = (SampleTable*) sample_alloc(sizeof(SampleTable));

    table->name = (char*) sample_alloc(strlen(name)+1);
    strcpy(table->name, name);

    table->width = width;
    table->rate  = rate;
    table->limit = limit;
    table->rows  = list_alloc();

    pthread_mutex_init(&table->mutex, NULL);

    thr_lock_init(&table->mysql_lock);
  }

  return table;
}

static void sample_table_drop(SampleTable *table, bool hard)
{
  if (hard)
  {
    char fname[1024];
    snprintf(fname, sizeof(fname), "%s.sample", table->name);
    remove(fname);
  }

  pthread_mutex_destroy(&table->mutex);
  list_free(table->rows);

  thr_lock_delete(&table->mysql_lock);
  list_delete(sample_tables, table);
  sample_free(table->name);
  sample_free(table);
}

static uchar* sample_field(SampleTable *table, uchar *row, uint field)
{
  sample_assert(field < table->width, "impossible field offset");

  uint offset = 0;
  for (uint col = 0; col < field; col++)
  {
    uchar type = row[offset++];
    switch (type) {
      case SAMPLE_NULL:
        break;
      case SAMPLE_STRING:
        offset += *((uint*)&row[offset]) + sizeof(uint) + sizeof(uint);
        break;
      case SAMPLE_TINYSTRING:
        offset += *((uint8_t*)&row[offset]) + sizeof(uint8_t);
        break;
      case SAMPLE_INT64:
        offset += sizeof(int64_t);
        break;
      case SAMPLE_INT32:
        offset += sizeof(int32_t);
        break;
      case SAMPLE_INT08:
        offset += sizeof(int8_t);
        break;
    }
  }
  return &row[offset];
}

static uchar sample_field_type(uchar *row)
{
  return *row;
}

static uchar* sample_field_buffer(uchar *row)
{
  uchar type = *row++;
  switch (type) {
    case SAMPLE_STRING:
      row += sizeof(uint) + sizeof(uint);
      break;
    case SAMPLE_TINYSTRING:
      row += sizeof(uint8_t);
      break;
    case SAMPLE_NULL:
    case SAMPLE_INT64:
    case SAMPLE_INT32:
    case SAMPLE_INT08:
      break;
  }
  return row;
}

static uint sample_field_length(uchar *row)
{
  uint length = 0;
  uchar type = *row++;
  switch (type) {
    case SAMPLE_NULL:
      break;
    case SAMPLE_STRING:
      length = *((uint*)row);
      break;
    case SAMPLE_TINYSTRING:
      length = *((uint8_t*)row);
      break;
    case SAMPLE_INT64:
      length = sizeof(int64_t);
      break;
    case SAMPLE_INT32:
      length = sizeof(int32_t);
      break;
    case SAMPLE_INT08:
      length = sizeof(int8_t);
      break;
  }
  return length;
}

static uint64 sample_field_width(uchar *row)
{
  uint64 length = 1;
  uchar type = *row++;
  switch (type) {
    case SAMPLE_NULL:
      break;
    case SAMPLE_STRING:
      length += *((uint*)row) + sizeof(uint) + sizeof(uint);
      break;
    case SAMPLE_TINYSTRING:
      length += *((uint8_t*)row) + sizeof(uint8_t);
      break;
    case SAMPLE_INT64:
      length += sizeof(int64_t);
      break;
    case SAMPLE_INT32:
      length += sizeof(int32_t);
      break;
    case SAMPLE_INT08:
      length += sizeof(int8_t);
      break;
  }
  return length;
}

static bool sample_show_status(handlerton* hton, THD* thd, stat_print_fn* stat_print, enum ha_stat_type stat_type)
{
  str_t *str = str_alloc(100);

  str_print(str, "hello");

  stat_print(thd, STRING_WITH_LEN("SAMPLE"), STRING_WITH_LEN("stuff"), str->buffer, str->length);

  str_reset(str);
  str_free(str);

  return FALSE; // success
}

static int sample_init_func(void *p)
{
  sample_hton = (handlerton*)p;

  sample_hton->state  = SHOW_OPTION_YES;
  sample_hton->create = sample_create_handler;

  sample_hton->flags
  = HTON_CAN_RECREATE
  | HTON_TEMPORARY_NOT_SUPPORTED
  | HTON_NO_PARTITION
  | HTON_SUPPORT_LOG_TABLES;

  sample_hton->table_options = sample_table_option_list;
  sample_hton->field_options = sample_field_option_list;
  sample_hton->show_status = sample_show_status;

  sample_seed = 1;

  pthread_mutex_init(&sample_tables_mutex, NULL);
  pthread_mutex_init(&sample_seed_mutex, NULL);
  pthread_mutex_init(&sample_stats_mutex, NULL);

  sample_tables = list_alloc();

  return 0;
}

static int sample_done_func(void *p)
{
  pthread_mutex_destroy(&sample_tables_mutex);
  pthread_mutex_destroy(&sample_seed_mutex);
  pthread_mutex_destroy(&sample_stats_mutex);

  while (!list_is_empty(sample_tables))
    sample_table_drop((SampleTable*)list_remove_head(sample_tables), FALSE);
  list_free(sample_tables);

  return 0;
}

static handler* sample_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root)
{
  return new (mem_root) ha_sample(hton, table);
}

ha_sample::ha_sample(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg)
{
  sample_debug("%s", __func__);
  sample_table = NULL;
  sample_trash = NULL;
  sample_rows  = NULL;
  sample_row   = NULL;
}

static const char *ha_sample_exts[] = {
  NullS
};

const char **ha_sample::bas_ext() const
{
  sample_debug("%s", __func__);
  return ha_sample_exts;
}

void ha_sample::empty_trash()
{
  if (sample_trash)
  {
    while (sample_trash->length)
      sample_free(list_remove_head(sample_trash));

    list_free(sample_trash);
    sample_trash = NULL;
  }
}

void ha_sample::use_trash()
{
  if (!sample_trash)
    sample_trash = list_alloc();
}

int ha_sample::open(const char *name, int mode, uint test_if_locked)
{
  sample_debug("%s %s", __func__, name);
  reset();

  pthread_mutex_lock(&sample_tables_mutex);

  sample_table = sample_table_open(name, table->s->fields, sample_rate, sample_limit);
  thr_lock_data_init(&sample_table->mysql_lock, &lock, NULL);
  sample_table->users++;

  pthread_mutex_unlock(&sample_tables_mutex);

  pthread_mutex_lock(&sample_seed_mutex);
  srand48_r(sample_seed++, &sample_rand);
  pthread_mutex_unlock(&sample_seed_mutex);

  counter_rows_inserted = 0;

  return sample_table ? 0: -1;
}

int ha_sample::close(void)
{
  sample_debug("%s", __func__);

  pthread_mutex_lock(&sample_tables_mutex);

  sample_table->users--;
  sample_table = NULL;

  pthread_mutex_unlock(&sample_tables_mutex);

  empty_trash();

  pthread_mutex_lock(&sample_stats_mutex);
  sample_counter_rows_inserted += counter_rows_inserted;
  pthread_mutex_unlock(&sample_stats_mutex);

  return 0;
}

int ha_sample::record_store(SampleRow *row, uchar *buf)
{
  if (!row)
    return HA_ERR_END_OF_FILE;

  memset(buf, 0, table->s->null_bytes);
  // Avoid asserts in ::store() for columns that are not going to be updated
  my_bitmap_map *org_bitmap = dbug_tmp_use_all_columns(table, table->write_set);

  uchar *buff = row->buffer;

  for (uint col = 0; col < sample_table->width; col++)
  {
    Field *field = table->field[col];

    uchar  type   = sample_field_type(buff);
    uchar *buffer = sample_field_buffer(buff);
    uint   length = sample_field_length(buff);

    switch (type) {
      case SAMPLE_NULL:
        field->set_null();
        break;
      case SAMPLE_STRING:
        field->store((char*)buffer, length, &my_charset_bin, CHECK_FIELD_WARN);
        break;
      case SAMPLE_TINYSTRING:
        field->store((char*)buffer, length, &my_charset_bin, CHECK_FIELD_WARN);
        break;
      case SAMPLE_INT64:
        field->store(*((int64_t*)buffer), FALSE);
        break;
      case SAMPLE_INT32:
        field->store(*((int32_t*)buffer), FALSE);
        break;
      case SAMPLE_INT08:
        field->store(*((int8_t*)buffer), FALSE);
        break;
    }

    buff += sample_field_width(buff);
  }
  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
  return 0;

  return 0;
}

SampleRow* ha_sample::record_place(uchar *buf)
{
  SampleRow *row = (SampleRow*) sample_alloc(sizeof(SampleRow));

  size_t length = 0;

  uint real_lengths[table->s->fields];
  memset(real_lengths, 0, sizeof(real_lengths));

  uint comp_lengths[table->s->fields];
  memset(comp_lengths, 0, sizeof(comp_lengths));

  uchar *compressed[table->s->fields];
  memset(compressed, 0, sizeof(compressed));

  for (uint col = 0; col < table->s->fields; col++)
  {
    Field *field = table->field[col];

    length += 1;

    if (field->is_null())
    {
      // nop
    }
    else
    if (field->result_type() == INT_RESULT)
    {
      if (field->val_int() < 128 && field->val_int() > -128)
        length += sizeof(int8_t);
      else
        length += sizeof(int64);
    }
    else
    {
      char pad[1024];
      String tmp(pad, sizeof(pad), &my_charset_bin);
      field->val_str(&tmp, &tmp);

      if (tmp.length() < 256)
      {
        real_lengths[col] = tmp.length();
        length += tmp.length() + sizeof(uint8_t);
      }
      else
      {
        real_lengths[col] = tmp.length();
        comp_lengths[col] = real_lengths[col];
        compressed[col] = (uchar*) sample_alloc(real_lengths[col]);
        memmove(compressed[col], tmp.ptr(), real_lengths[col]);
        length += comp_lengths[col] + sizeof(uint) + sizeof(uint);
      }
    }
  }

  row->buffer = (uchar*) sample_alloc(length);
  row->length = length;

  for (uint col = 0; col < table->s->fields; col++)
  {
    Field *field = table->field[col];
    uchar *buff = sample_field(sample_table, row->buffer, col);

    if (field->is_null())
    {
      *buff = SAMPLE_NULL;
    }
    else
    if (field->result_type() == INT_RESULT)
    {
      if (field->val_int() < 128 && field->val_int() > -128)
      {
        *buff++ = SAMPLE_INT08;
        *((int8_t*)buff) = field->val_int();
      }
      else
      if (field->val_int() < INT_MAX && field->val_int() > INT_MIN)
      {
        *buff++ = SAMPLE_INT32;
        *((int32_t*)buff) = field->val_int();
      }
      else
      {
        *buff++ = SAMPLE_INT64;
        *((int64_t*)buff) = field->val_int();
      }
    }
    else
    {
      if (!compressed[col])
      {
        char pad[1024];
        String tmp(pad, sizeof(pad), &my_charset_bin);
        field->val_str(&tmp, &tmp);

        *buff++ = SAMPLE_TINYSTRING;
        *((uint8_t*)buff) = tmp.length();
        buff += sizeof(uint8_t);
        memmove(buff, tmp.ptr(), tmp.length());
      }
      else
      {
        *buff++ = SAMPLE_STRING;
        *((uint*)buff) = comp_lengths[col];
        buff += sizeof(uint);
        *((uint*)buff) = real_lengths[col];
        buff += sizeof(uint);
        memmove(buff, compressed[col], comp_lengths[col]);
      }
    }

    if (compressed[col])
      sample_free(compressed[col]);
  }

  return row;
}

int ha_sample::write_row(uchar *buf)
{
  sample_debug("%s", __func__);

  long r; lrand48_r(&sample_rand, &r);
  bool complete = r % sample_table->rate == 0;

  if (complete)
  {
    // Avoid asserts in val_str() for columns that are not going to be updated
    my_bitmap_map *org_bitmap = dbug_tmp_use_all_columns(table, table->read_set);

    SampleRow *row = record_place(buf);

    bool inserted = FALSE;

    if (pthread_mutex_trylock(&sample_table->mutex) == 0)
    {
      if (sample_table->limit > sample_table->rows->length)
      {
        list_insert_head(sample_table->rows, row);
        inserted = TRUE;
      }
      pthread_mutex_unlock(&sample_table->mutex);
    }

    if (!inserted)
    {
      sample_free(row->buffer);
      sample_free(row);
    }

    dbug_tmp_restore_column_map(table->read_set, org_bitmap);
    counter_rows_inserted++;
  }
  return 0;
}

int ha_sample::update_row(const uchar *old_data, uchar *new_data)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_sample::delete_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_sample::rnd_init(bool scan)
{
  sample_debug("%s", __func__);
  rnd_end();
  return 0;
}

int ha_sample::rnd_end()
{
  sample_debug("%s", __func__);

  if (sample_row)
  {
    sample_free(sample_row->buffer);
    sample_free(sample_row);
  }

  while (sample_rows && sample_rows->length && (sample_row = (SampleRow*) list_remove_head(sample_rows)))
  {
    sample_free(sample_row->buffer);
    sample_free(sample_row);
  }

  list_free(sample_rows);
  sample_rows = NULL;
  sample_row  = NULL;

  return 0;
}

int ha_sample::rnd_next(uchar *buf)
{
  sample_debug("%s", __func__);

  if (!sample_rows)
  {
    pthread_mutex_lock(&sample_table->mutex);
    sample_rows = sample_table->rows;
    sample_table->rows = list_alloc();
    pthread_mutex_unlock(&sample_table->mutex);
  }

  if (sample_row)
  {
    sample_free(sample_row->buffer);
    sample_free(sample_row);
    sample_row = NULL;
  }

  if (sample_rows && sample_rows->length)
    sample_row = (SampleRow*) list_remove_head(sample_rows);

  return record_store(sample_row, buf);
}

int ha_sample::index_init(uint idx, bool sorted)
{
  sample_debug("%s", __func__);
  return HA_ERR_WRONG_COMMAND;
}

int ha_sample::index_read(uchar * buf, const uchar * key, uint key_len, enum ha_rkey_function find_flag)
{
  sample_debug("%s", __func__);
  return HA_ERR_WRONG_COMMAND;
}

int ha_sample::index_end()
{
  sample_debug("%s", __func__);
  return HA_ERR_WRONG_COMMAND;
}

void ha_sample::position(const uchar *record)
{
}

int ha_sample::rnd_pos(uchar *buf, uchar *pos)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_sample::info(uint flag)
{
  sample_debug("%s", __func__);
  return 0;
}

int ha_sample::reset()
{
  sample_debug("%s", __func__);
  return 0;
}

int ha_sample::external_lock(THD *thd, int lock_type)
{
  sample_debug("%s", __func__);
  return 0;
}

int ha_sample::delete_table(const char *name)
{
  sample_debug("%s %s", __func__, name);

  pthread_mutex_lock(&sample_tables_mutex);

  SampleTable *table = sample_table_open(name, 0, 0, 0);

  if (table && !table->dropping)
  {
    table->users++;
    table->dropping = TRUE;
    while (table->users > 1)
    {
      pthread_mutex_unlock(&sample_tables_mutex);
      usleep(1000);
      pthread_mutex_lock(&sample_tables_mutex);
    }
    sample_table_drop(table, TRUE);
  }

  pthread_mutex_unlock(&sample_tables_mutex);

  return 0;
}

int ha_sample::rename_table(const char *from, const char *to)
{
  sample_debug("%s %s %s", __func__, from, to);

  pthread_mutex_lock(&sample_tables_mutex);

  SampleTable *table = sample_table_open(from, 0, 0, 0);

  if (table)
  {
    if (sample_table != table)
      table->users++;

    while (table->users > 1)
    {
      pthread_mutex_unlock(&sample_tables_mutex);
      usleep(1000);
      pthread_mutex_lock(&sample_tables_mutex);
    }

    sample_free(table->name);
    table->name = (char*) sample_alloc(strlen(to)+1);
    strcpy(table->name, to);

    if (sample_table != table)
      table->users--;
  }

  pthread_mutex_unlock(&sample_tables_mutex);
  return 0;
}

int ha_sample::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
  sample_debug("%s %s", __func__, name);
  return 0;
}

bool ha_sample::check_if_incompatible_data(HA_CREATE_INFO *info, uint table_changes)
{
  sample_debug("%s", __func__);
  return COMPATIBLE_DATA_NO;
}

THR_LOCK_DATA **ha_sample::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
  {
    /*
      If TL_UNLOCK is set
      If we are not doing a LOCK TABLE or DISCARD/IMPORT
      TABLESPACE, then allow multiple writers
    */

    if ((lock_type >= TL_WRITE_CONCURRENT_INSERT &&
         lock_type <= TL_WRITE) && !thd->in_lock_tables
        && !thd->tablespace_op)
      lock_type = TL_WRITE_ALLOW_WRITE;

    /*
      In queries of type INSERT INTO t1 SELECT ... FROM t2 ...
      MySQL would use the lock TL_READ_NO_INSERT on t2, and that
      would conflict with TL_WRITE_ALLOW_WRITE, blocking all inserts
      to t2. Convert the lock to a normal read lock to allow
      concurrent inserts to t2.
    */

    if (lock_type == TL_READ_NO_INSERT && !thd->in_lock_tables)
      lock_type = TL_READ;
  }
  *to++ = &lock;
  return to;
}

struct st_mysql_storage_engine sample_storage_engine= { MYSQL_HANDLERTON_INTERFACE_VERSION };

static void sample_verbose_update(THD * thd, struct st_mysql_sys_var *sys_var, void *var, const void *save)
{
  uint n = *((uint*)save);
  *((uint*)var) = n;
  sample_verbose = n;
}

static void sample_rate_update(THD * thd, struct st_mysql_sys_var *sys_var, void *var, const void *save)
{
  uint n = *((uint*)save);
  *((uint*)var) = n;
  sample_rate = n;
}

static void sample_limit_update(THD * thd, struct st_mysql_sys_var *sys_var, void *var, const void *save)
{
  uint n = *((uint*)save);
  *((uint*)var) = n;
  sample_limit = n;
}

static MYSQL_SYSVAR_UINT(verbose, sample_verbose, 0,
  "Debug noise to stderr.", 0, sample_verbose_update, 0, 0, 1, 1);

static MYSQL_SYSVAR_UINT(rate, sample_rate, 0,
  "Sample rate.", 0, sample_rate_update, 1000, 1, UINT_MAX, 1);

static MYSQL_SYSVAR_UINT(limit, sample_limit, 0,
  "Table rows limit.", 0, sample_limit_update, 10000, 1, UINT_MAX, 1);

static struct st_mysql_sys_var *sample_system_variables[] = {
    MYSQL_SYSVAR(verbose),
    MYSQL_SYSVAR(rate),
    MYSQL_SYSVAR(limit),
    NULL
};

static struct st_mysql_show_var func_status[]=
{
  { "sample_counter_rows_inserted", (char*)&sample_counter_rows_inserted, SHOW_ULONGLONG },
  { 0,0,SHOW_UNDEF }
};

struct st_mysql_daemon unusable_sample=
{ MYSQL_DAEMON_INTERFACE_VERSION };

mysql_declare_plugin(sample)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &sample_storage_engine,
  "SAMPLE",
  "Sean Pringle, Wikimedia Foundation",
  "Sample everything into memory!",
  PLUGIN_LICENSE_GPL,
  sample_init_func,                               /* Plugin Init */
  sample_done_func,                               /* Plugin Deinit */
  0x0001 /* 0.1 */,
  func_status,                                  /* status variables */
  sample_system_variables,                        /* system variables */
  NULL,                                         /* config options */
  0,                                            /* flags */
}
mysql_declare_plugin_end;
maria_declare_plugin(sample)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &sample_storage_engine,
  "SAMPLE",
  "Sean Pringle, Wikimedia Foundation",
  "Sample everything into memory!",
  PLUGIN_LICENSE_GPL,
  sample_init_func,                               /* Plugin Init */
  sample_done_func,                               /* Plugin Deinit */
  0x0001,                                       /* version number (0.1) */
  func_status,                                  /* status variables */
  sample_system_variables,                        /* system variables */
  "0.1",                                        /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
},
{
  MYSQL_DAEMON_PLUGIN,
  &unusable_sample,
  "SAMPLE UNUSABLE",
  "Sean Pringle",
  "Unusable Engine",
  PLUGIN_LICENSE_GPL,
  NULL,                                         /* Plugin Init */
  NULL,                                         /* Plugin Deinit */
  0x0100,                                       /* version number (1.00) */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "1.00",                                       /* version, as a string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;
