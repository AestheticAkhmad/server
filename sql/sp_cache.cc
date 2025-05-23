/* Copyright (c) 2002, 2012, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sp_cache.h"
#include "sp_head.h"

static mysql_mutex_t Cversion_lock;
static ulong volatile Cversion= 1;


/*
  Cache of stored routines.
*/

class sp_cache
{
public:
  sp_cache();
  ~sp_cache();

  /**
   Inserts a sp_head object into a hash table.

   @returns Success status
     @return TRUE Failure
     @return FALSE Success
  */
  inline bool insert(sp_head *sp)
  {
    return my_hash_insert(&m_hashtable, (const uchar *)sp);
  }

  inline sp_head *lookup(char *name, size_t namelen)
  {
    return (sp_head *) my_hash_search(&m_hashtable, (const uchar *)name,
                                      namelen);
  }

  inline void remove(sp_head *sp)
  {
    my_hash_delete(&m_hashtable, (uchar *)sp);
  }

  /**
    Remove all elements from a stored routine cache if the current
    number of elements exceeds the argument value.

    @param[in] upper_limit_for_elements  Soft upper limit of elements that
                                         can be stored in the cache.
  */
  void enforce_limit(ulong upper_limit_for_elements)
  {
    if (m_hashtable.records > upper_limit_for_elements)
      my_hash_reset(&m_hashtable);
  }

private:
  void init();
  void cleanup();

  /* All routines in this cache */
  HASH m_hashtable;
public:
  void clear();
}; // class sp_cache

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_Cversion_lock;

static PSI_mutex_info all_sp_cache_mutexes[]=
{
  { &key_Cversion_lock, "Cversion_lock", PSI_FLAG_GLOBAL}
};

static void init_sp_cache_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_sp_cache_mutexes);
  PSI_server->register_mutex(category, all_sp_cache_mutexes, count);
}
#endif

/* Initialize the SP caching once at startup */

void sp_cache_init()
{
#ifdef HAVE_PSI_INTERFACE
  init_sp_cache_psi_keys();
#endif

  mysql_mutex_init(key_Cversion_lock, &Cversion_lock, MY_MUTEX_INIT_FAST);
}


/*
  Clear the cache *cp and set *cp to NULL.

  SYNOPSIS
    sp_cache_clear()
    cp  Pointer to cache to clear

  NOTE
    This function doesn't invalidate other caches.
*/

void sp_cache_clear(sp_cache **cp)
{
  sp_cache *c= *cp;

  if (c)
  {
    delete c;
    *cp= NULL;
  }
}


void sp_cache_end()
{
  mysql_mutex_destroy(&Cversion_lock);
}


/*
  Insert a routine into the cache.

  SYNOPSIS
    sp_cache_insert()
     cp  The cache to put routine into
     sp  Routine to insert.

  TODO: Perhaps it will be more straightforward if in case we returned an
        error from this function when we couldn't allocate sp_cache. (right
        now failure to put routine into cache will cause a 'SP not found'
        error to be reported at some later time)
*/

void sp_cache_insert(sp_cache **cp, sp_head *sp)
{
  sp_cache *c;

  if (!(c= *cp))
  {
    if (!(c= new sp_cache()))
      return;                                   // End of memory error
  }
  /* Reading a ulong variable with no lock. */
  sp->set_sp_cache_version(Cversion);
  DBUG_PRINT("info",("sp_cache: inserting: %s", ErrConvDQName(sp).ptr()));
  c->insert(sp);
  *cp= c;                                       // Update *cp if it was NULL
}


/*
  Look up a routine in the cache.
  SYNOPSIS
    sp_cache_lookup()
      cp    Cache to look into
      name  Name of routine to find

  NOTE
    An obsolete (but not more obsolete then since last
    sp_cache_flush_obsolete call) routine may be returned.

  RETURN
    The routine or
    NULL if the routine not found.
*/

sp_head *sp_cache_lookup(sp_cache **cp, const Database_qualified_name *name)
{
  char buf[NAME_LEN * 2 + 2];
  sp_cache *c= *cp;
  if (! c)
    return NULL;
  return c->lookup(buf, name->to_identifier_chain2().
                          make_qname_casedn_part1(buf, sizeof(buf)));
}


/*
  Invalidate all routines in all caches.

  SYNOPSIS
    sp_cache_invalidate()

  NOTE
    This is called when a VIEW definition is created or modified (and in some
    other contexts). We can't destroy sp_head objects here as one may modify
    VIEW definitions from prelocking-free SPs.
*/

void sp_cache_invalidate()
{
  DBUG_PRINT("info",("sp_cache: invalidating"));
  thread_safe_increment(Cversion, &Cversion_lock);
}


/**
  Remove an out-of-date SP from the cache.

  @param[in] cp  Cache to flush
  @param[in] sp  SP to remove.

  @note This invalidates pointers to sp_head objects this thread
  uses. In practice that means don't call this function when
  inside SP'.
*/

void sp_cache_flush_obsolete(sp_cache **cp, sp_head **sp)
{
  if ((*sp)->sp_cache_version() < Cversion && !(*sp)->is_invoked())
  {
    DBUG_EXECUTE_IF("check_sp_cache_not_invalidated", DBUG_SUICIDE(););
    (*cp)->remove(*sp);
    *sp= NULL;
  }
}

/**
  Return the current global version of the cache.
*/

ulong sp_cache_version()
{
  return Cversion;
}


/**
  Enforce that the current number of elements in the cache don't exceed
  the argument value by flushing the cache if necessary.

  @param[in] c  Cache to check
  @param[in] upper_limit_for_elements  Soft upper limit for number of sp_head
                                       objects that can be stored in the cache.
*/
void
sp_cache_enforce_limit(sp_cache *c, ulong upper_limit_for_elements)
{
 if (c)
   c->enforce_limit(upper_limit_for_elements);
}

/*************************************************************************
  Internal functions
 *************************************************************************/

extern "C" const uchar *hash_get_key_for_sp_head(const void *ptr, size_t *plen,
                                                 my_bool);
extern "C" void hash_free_sp_head(void *p);

const uchar *hash_get_key_for_sp_head(const void *ptr, size_t *plen, my_bool)
{
  auto sp= static_cast<const sp_head *>(ptr);
  *plen= sp->m_qname.length;
  return reinterpret_cast<const uchar *>(sp->m_qname.str);
}


void hash_free_sp_head(void *p)
{
  sp_head *sp= (sp_head *)p;
  sp_head::destroy(sp);
}


sp_cache::sp_cache()
{
  init();
}


sp_cache::~sp_cache()
{
  my_hash_free(&m_hashtable);
}


void
sp_cache::init()
{
  my_hash_init(key_memory_sp_cache, &m_hashtable, &my_charset_bin, 0, 0, 0,
               hash_get_key_for_sp_head, hash_free_sp_head, 0);
}


void
sp_cache::cleanup()
{
  my_hash_free(&m_hashtable);
}

void sp_cache::clear()
{
  my_hash_reset(&m_hashtable);
}

void Sp_caches::sp_caches_clear()
{
  sp_cache_clear(&sp_proc_cache);
  sp_cache_clear(&sp_func_cache);
  sp_cache_clear(&sp_package_spec_cache);
  sp_cache_clear(&sp_package_body_cache);
}

void Sp_caches::sp_caches_empty()
{
  if (sp_proc_cache)
    sp_proc_cache->clear();
  if (sp_func_cache)
    sp_func_cache->clear();
  if (sp_package_spec_cache)
    sp_package_spec_cache->clear();
  if (sp_package_body_cache)
    sp_package_body_cache->clear();
}
