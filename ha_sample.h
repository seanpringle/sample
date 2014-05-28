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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#define MYSQL_SERVER 1 // required for THD class
#include "my_global.h"
#include <sql_table.h>
#include <sql_class.h>
#include <probes_mysql.h>
#include "thr_lock.h" /* THR_LOCK, THR_LOCK_DATA */

typedef bool (*map_fn)(void*, void*);
typedef int (*cmp_fn)(void*, void*);

typedef struct str_st {
  char *buffer;
  size_t length, limit;
} str_t;

typedef struct node_st {
  void *payload;
  struct node_st *next;
} node_t;

typedef struct list_st {
  node_t *head;
  uint64 length;
} list_t;

typedef struct _SampleTable {
  char *name;
  uint users;
  uint width;
  uint rate;
  bool dropping;
  pthread_mutex_t mutex;
  uint limit;
  list_t *rows;
  THR_LOCK mysql_lock;
} SampleTable;

typedef struct _SampleRow {
  uchar *buffer;
  uint length;
} SampleRow;

enum {
  SAMPLE_NULL=1,
  SAMPLE_INT08,
  SAMPLE_INT32,
  SAMPLE_INT64,
  SAMPLE_STRING,
  SAMPLE_TINYSTRING,
};

/** @brief
  Class definition for the storage engine
*/
class ha_sample: public handler
{
  THR_LOCK_DATA lock;
  SampleTable *sample_table;
  list_t *sample_trash;

  list_t *sample_rows;
  SampleRow *sample_row;

  uint counter_rows_inserted;

  struct drand48_data sample_rand;

public:
  ha_sample(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_sample()
  {
  }

  const char *table_type() const { return "SAMPLE"; }
  const char **bas_ext() const;

  ulonglong table_flags() const
  {
    return (
        HA_NO_TRANSACTIONS
      | HA_NO_AUTO_INCREMENT
      | HA_REC_NOT_IN_SEQ
      | HA_BINLOG_ROW_CAPABLE
      | HA_BINLOG_STMT_CAPABLE
    );
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return HA_DO_INDEX_COND_PUSHDOWN;
  }

  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys()          const { return 0; }
  uint max_supported_key_parts()     const { return 0; }
  uint max_supported_key_length()    const { return UINT_MAX; }
  virtual double scan_time() { return (double) DBL_MIN; }
  virtual double read_time(uint, uint, ha_rows rows) { return (double) DBL_MAX/2; }
  int open(const char *name, int mode, uint test_if_locked);    // required
  int close(void);                                              // required
  int write_row(uchar *buf);
  int update_row(const uchar *old_data, uchar *new_data);
  int delete_row(const uchar *buf);
  int rnd_init(bool scan);                                      //required
  int rnd_end();
  int rnd_next(uchar *buf);                                     ///< required
  int rnd_pos(uchar *buf, uchar *pos);                          ///< required
  int index_init(uint idx, bool sorted);
  int index_read(uchar * buf, const uchar * key, uint key_len, enum ha_rkey_function find_flag);
  int index_end();
  void position(const uchar *record);                           ///< required
  int info(uint);                                               ///< required
  int reset();
  int external_lock(THD *thd, int lock_type);                   ///< required
  int delete_table(const char *from);
  int rename_table(const char *from, const char *to);
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info);                      ///< required
  bool check_if_incompatible_data(HA_CREATE_INFO *info, uint table_changes);
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type);     ///< required
  int record_store(SampleRow *row, uchar *buf);
  SampleRow* record_place(uchar *buf);

  void empty_trash();
  void use_trash();

};
