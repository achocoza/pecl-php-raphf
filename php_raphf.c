/*
    +--------------------------------------------------------------------+
    | PECL :: raphf                                                      |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted provided that the conditions mentioned |
    | in the accompanying LICENSE file are met.                          |
    +--------------------------------------------------------------------+
    | Copyright (c) 2013, Michael Wallner <mike@php.net>                 |
    +--------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_raphf.h"

#ifndef PHP_RAPHF_TEST
#	define PHP_RAPHF_TEST 0
#endif

struct php_persistent_handle_globals {
	ulong limit;
	HashTable hash;
};

ZEND_BEGIN_MODULE_GLOBALS(raphf)
	struct php_persistent_handle_globals persistent_handle;
ZEND_END_MODULE_GLOBALS(raphf)

#ifdef ZTS
#	define PHP_RAPHF_G ((zend_raphf_globals *) \
		(*((void ***) tsrm_ls))[TSRM_UNSHUFFLE_RSRC_ID(raphf_globals_id)])
#else
#	define PHP_RAPHF_G (&raphf_globals)
#endif

ZEND_DECLARE_MODULE_GLOBALS(raphf)

#ifndef PHP_RAPHF_DEBUG_PHANDLES
#	define PHP_RAPHF_DEBUG_PHANDLES 0
#endif
#if PHP_RAPHF_DEBUG_PHANDLES
#	undef inline
#	define inline
#endif

php_resource_factory_t *php_resource_factory_init(php_resource_factory_t *f,
		php_resource_factory_ops_t *fops, void *data, void (*dtor)(void *data))
{
	if (!f) {
		f = emalloc(sizeof(*f));
	}
	memset(f, 0, sizeof(*f));

	memcpy(&f->fops, fops, sizeof(*fops));

	f->data = data;
	f->dtor = dtor;

	f->refcount = 1;

	return f;
}

unsigned php_resource_factory_addref(php_resource_factory_t *rf)
{
	return ++rf->refcount;
}

void php_resource_factory_dtor(php_resource_factory_t *f)
{
	if (!--f->refcount) {
		if (f->dtor) {
			f->dtor(f->data);
		}
	}
}

void php_resource_factory_free(php_resource_factory_t **f)
{
	if (*f) {
		php_resource_factory_dtor(*f);
		if (!(*f)->refcount) {
			efree(*f);
			*f = NULL;
		}
	}
}

void *php_resource_factory_handle_ctor(php_resource_factory_t *f,
		void *init_arg TSRMLS_DC)
{
	if (f->fops.ctor) {
		return f->fops.ctor(f->data, init_arg TSRMLS_CC);
	}
	return NULL;
}

void *php_resource_factory_handle_copy(php_resource_factory_t *f,
		void *handle TSRMLS_DC)
{
	if (f->fops.copy) {
		return f->fops.copy(f->data, handle TSRMLS_CC);
	}
	return NULL;
}

void php_resource_factory_handle_dtor(php_resource_factory_t *f,
		void *handle TSRMLS_DC)
{
	if (f->fops.dtor) {
		f->fops.dtor(f->data, handle TSRMLS_CC);
	}
}

static inline php_persistent_handle_list_t *php_persistent_handle_list_init(
		php_persistent_handle_list_t *list)
{
	if (!list) {
		list = pemalloc(sizeof(*list), 1);
	}
	list->used = 0;
	zend_hash_init(&list->free, 0, NULL, NULL, 1);

	return list;
}

static int php_persistent_handle_apply_stat(zval *p TSRMLS_DC, int argc,
		va_list argv, zend_hash_key *key)
{
	php_persistent_handle_list_t *list = Z_PTR_P(p);
	zval zsubentry, *zentry = va_arg(argv, zval *);

	array_init(&zsubentry);
	add_assoc_long_ex(&zsubentry, ZEND_STRL("used"), list->used);
	add_assoc_long_ex(&zsubentry, ZEND_STRL("free"),
			zend_hash_num_elements(&list->free));
	if (key->key) {
		add_assoc_zval_ex(zentry, key->key->val, key->key->len, &zsubentry);
	} else {
		add_index_zval(zentry, key->h, &zsubentry);
	}
	return ZEND_HASH_APPLY_KEEP;
}

static int php_persistent_handle_apply_statall(zval *p TSRMLS_DC, int argc,
		va_list argv, zend_hash_key *key)
{
	php_persistent_handle_provider_t *provider = Z_PTR_P(p);
	HashTable *ht = va_arg(argv, HashTable *);
	zval zentry;

	array_init(&zentry);

	zend_hash_apply_with_arguments(&provider->list.free TSRMLS_CC,
			php_persistent_handle_apply_stat, 1, &zentry);

	if (key->key) {
		zend_hash_update(ht, key->key, &zentry);
	} else {
		zend_hash_index_update(ht, key->h, &zentry);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static int php_persistent_handle_apply_cleanup_ex(zval *p, void *arg TSRMLS_DC)
{
	php_resource_factory_t *rf = arg;
	void *handle = Z_PTR_P(p);

#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "DESTROY: %p\n", handle);
#endif
	php_resource_factory_handle_dtor(rf, handle TSRMLS_CC);
	return ZEND_HASH_APPLY_REMOVE;
}

static int php_persistent_handle_apply_cleanup(zval *p, void *arg TSRMLS_DC)
{
	php_resource_factory_t *rf = arg;
	php_persistent_handle_list_t *list = Z_PTR_P(p);

	zend_hash_apply_with_argument(&list->free,
			php_persistent_handle_apply_cleanup_ex, rf TSRMLS_CC);
	if (list->used) {
		return ZEND_HASH_APPLY_KEEP;
	}
	zend_hash_destroy(&list->free);
#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "LSTFREE: %p\n", list);
#endif
	pefree(list, 1);
	return ZEND_HASH_APPLY_REMOVE;
}

static inline void php_persistent_handle_list_dtor(
		php_persistent_handle_list_t *list,
		php_persistent_handle_provider_t *provider TSRMLS_DC)
{
#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "LSTDTOR: %p\n", list);
#endif
	zend_hash_apply_with_argument(&list->free,
			php_persistent_handle_apply_cleanup_ex, &provider->rf TSRMLS_CC);
	zend_hash_destroy(&list->free);
}

static inline void php_persistent_handle_list_free(
		php_persistent_handle_list_t **list,
		php_persistent_handle_provider_t *provider TSRMLS_DC)
{
	php_persistent_handle_list_dtor(*list, provider TSRMLS_CC);
#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "LSTFREE: %p\n", *list);
#endif
	pefree(*list, 1);
	*list = NULL;
}

static int php_persistent_handle_list_apply_dtor(zval *p,
		void *provider TSRMLS_DC)
{
	php_persistent_handle_list_t *list = Z_PTR_P(p);
	php_persistent_handle_list_free(&list, provider TSRMLS_CC);
	ZVAL_PTR(p, NULL);
	return ZEND_HASH_APPLY_REMOVE;
}

static inline php_persistent_handle_list_t *php_persistent_handle_list_find(
		php_persistent_handle_provider_t *provider, const char *ident_str,
		size_t ident_len TSRMLS_DC)
{
	php_persistent_handle_list_t *list;

	list = zend_symtable_str_find_ptr(&provider->list.free, ident_str, ident_len);

	if (list) {
#if PHP_RAPHF_DEBUG_PHANDLES
		fprintf(stderr, "LSTFIND: %p\n", list);
#endif
		return list;
	}

	if ((list = php_persistent_handle_list_init(NULL))) {
		zval p;

		ZVAL_PTR(&p, list);
		if (zend_symtable_str_update(&provider->list.free, ident_str, ident_len,
				&p)) {
#if PHP_RAPHF_DEBUG_PHANDLES
			fprintf(stderr, "LSTFIND: %p (new)\n", list);
#endif
			return list;
		}
		php_persistent_handle_list_free(&list, provider TSRMLS_CC);
	}

	return NULL;
}

static int php_persistent_handle_apply_cleanup_all(zval *p TSRMLS_DC, int argc,
		va_list argv, zend_hash_key *key)
{
	php_persistent_handle_provider_t *provider = Z_PTR_P(p);
	const char *ident_str = va_arg(argv, const char *);
	size_t ident_len = va_arg(argv, size_t);
	php_persistent_handle_list_t *list;

	if (ident_str && ident_len) {
		if ((list = php_persistent_handle_list_find(provider, ident_str,
				ident_len TSRMLS_CC))) {
			zend_hash_apply_with_argument(&list->free,
					php_persistent_handle_apply_cleanup_ex,
					&provider->rf TSRMLS_CC);
		}
	} else {
		zend_hash_apply_with_argument(&provider->list.free,
				php_persistent_handle_apply_cleanup, &provider->rf TSRMLS_CC);
	}

	return ZEND_HASH_APPLY_KEEP;
}

static void php_persistent_handle_hash_dtor(zval *p)
{
	php_persistent_handle_provider_t *provider = Z_PTR_P(p);
	TSRMLS_FETCH();

	zend_hash_apply_with_argument(&provider->list.free,
			php_persistent_handle_list_apply_dtor, provider TSRMLS_CC);
	zend_hash_destroy(&provider->list.free);
	php_resource_factory_dtor(&provider->rf);
	pefree(provider, 1);
}

ZEND_RESULT_CODE php_persistent_handle_provide(const char *name_str,
		size_t name_len, php_resource_factory_ops_t *fops, void *data,
		void (*dtor)(void *) TSRMLS_DC)
{
	php_persistent_handle_provider_t *provider = pemalloc(sizeof(*provider), 1);

	if (php_persistent_handle_list_init(&provider->list)) {
		if (php_resource_factory_init(&provider->rf, fops, data, dtor)) {
			zval p;

#if PHP_RAPHF_DEBUG_PHANDLES
			fprintf(stderr, "PROVIDE: %p %s\n", PHP_RAPHF_G, name_str);
#endif

			ZVAL_PTR(&p, provider);
			if (zend_symtable_str_update(&PHP_RAPHF_G->persistent_handle.hash,
					name_str, name_len, &p)) {
				return SUCCESS;
			}
			php_resource_factory_dtor(&provider->rf);
		}
	}

	return FAILURE;
}


php_persistent_handle_factory_t *php_persistent_handle_concede(
		php_persistent_handle_factory_t *a, const char *name_str,
		size_t name_len, const char *ident_str, size_t ident_len,
		php_persistent_handle_wakeup_t wakeup,
		php_persistent_handle_retire_t retire TSRMLS_DC)
{
	php_persistent_handle_factory_t *free_a = NULL;

	if (!a) {
		free_a = a = emalloc(sizeof(*a));
	}
	memset(a, 0, sizeof(*a));

	a->provider = zend_symtable_str_find_ptr(&PHP_RAPHF_G->persistent_handle.hash,
			name_str, name_len);

	if (a->provider) {
		a->ident.str = estrndup(ident_str, ident_len);
		a->ident.len = ident_len;

		a->wakeup = wakeup;
		a->retire = retire;

		if (free_a) {
			a->free_on_abandon = 1;
		}
	} else {
		if (free_a) {
			efree(free_a);
		}
		a = NULL;
	}

#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "CONCEDE: %p %p (%s) (%s)\n", PHP_RAPHF_G,
			a ? a->provider : NULL, name_str, ident_str);
#endif

	return a;
}

void php_persistent_handle_abandon(php_persistent_handle_factory_t *a)
{
	zend_bool f = a->free_on_abandon;

#if PHP_RAPHF_DEBUG_PHANDLES
	fprintf(stderr, "ABANDON: %p\n", a->provider);
#endif

	if (a->ident.str) {
		efree(a->ident.str);
	}
	memset(a, 0, sizeof(*a));
	if (f) {
		efree(a);
	}
}

void *php_persistent_handle_acquire(php_persistent_handle_factory_t *a,
		void *init_arg  TSRMLS_DC)
{
	int key;
	zval *p;
	ulong index;
	void *handle = NULL;
	php_persistent_handle_list_t *list;

	list = php_persistent_handle_list_find(a->provider, a->ident.str,
			a->ident.len TSRMLS_CC);
	if (list) {
		zend_hash_internal_pointer_end(&list->free);
		key = zend_hash_get_current_key(&list->free, NULL, &index, 0);
		p = zend_hash_get_current_data(&list->free);
		if (p && HASH_KEY_NON_EXISTENT != key) {
			handle = Z_PTR_P(p);
			if (a->wakeup) {
				a->wakeup(a, &handle TSRMLS_CC);
			}
			zend_hash_index_del(&list->free, index);
		} else {
			handle = php_resource_factory_handle_ctor(&a->provider->rf,
					init_arg TSRMLS_CC);
		}
#if PHP_RAPHF_DEBUG_PHANDLES
		fprintf(stderr, "CREATED: %p\n", handle);
#endif
		if (handle) {
			++a->provider->list.used;
			++list->used;
		}
	}

	return handle;
}

void *php_persistent_handle_accrete(php_persistent_handle_factory_t *a,
		void *handle TSRMLS_DC)
{
	void *new_handle = NULL;
	php_persistent_handle_list_t *list;

	new_handle = php_resource_factory_handle_copy(&a->provider->rf,
			handle TSRMLS_CC);
	if (handle) {
		list = php_persistent_handle_list_find(a->provider, a->ident.str,
				a->ident.len TSRMLS_CC);
		if (list) {
			++list->used;
		}
		++a->provider->list.used;
	}

	return new_handle;
}

void php_persistent_handle_release(php_persistent_handle_factory_t *a,
		void *handle TSRMLS_DC)
{
	php_persistent_handle_list_t *list;

	list = php_persistent_handle_list_find(a->provider, a->ident.str,
			a->ident.len TSRMLS_CC);
	if (list) {
		if (a->provider->list.used >= PHP_RAPHF_G->persistent_handle.limit) {
#if PHP_RAPHF_DEBUG_PHANDLES
			fprintf(stderr, "DESTROY: %p\n", handle);
#endif
			php_resource_factory_handle_dtor(&a->provider->rf, handle TSRMLS_CC);
		} else {
			if (a->retire) {
				a->retire(a, &handle TSRMLS_CC);
			}
			zend_hash_next_index_insert_ptr(&list->free, handle);
		}

		--a->provider->list.used;
		--list->used;
	}
}

void php_persistent_handle_cleanup(const char *name_str, size_t name_len,
		const char *ident_str, size_t ident_len TSRMLS_DC)
{
	php_persistent_handle_provider_t *provider;
	php_persistent_handle_list_t *list;

	if (name_str && name_len) {
		provider = zend_symtable_str_find_ptr(&PHP_RAPHF_G->persistent_handle.hash,
				name_str, name_len);

		if (provider) {
			if (ident_str && ident_len) {
				list = php_persistent_handle_list_find(provider, ident_str,
						ident_len TSRMLS_CC);
				if (list) {
					zend_hash_apply_with_argument(&list->free,
							php_persistent_handle_apply_cleanup_ex,
							&provider->rf TSRMLS_CC);
				}
			} else {
				zend_hash_apply_with_argument(&provider->list.free,
						php_persistent_handle_apply_cleanup,
						&provider->rf TSRMLS_CC);
			}
		}
	} else {
		zend_hash_apply_with_arguments(
				&PHP_RAPHF_G->persistent_handle.hash TSRMLS_CC,
				php_persistent_handle_apply_cleanup_all, 2, ident_str,
				ident_len);
	}
}

HashTable *php_persistent_handle_statall(HashTable *ht TSRMLS_DC)
{
	if (zend_hash_num_elements(&PHP_RAPHF_G->persistent_handle.hash)) {
		if (!ht) {
			ALLOC_HASHTABLE(ht);
			zend_hash_init(ht, 0, NULL, ZVAL_PTR_DTOR, 0);
		}
		zend_hash_apply_with_arguments(
				&PHP_RAPHF_G->persistent_handle.hash TSRMLS_CC,
				php_persistent_handle_apply_statall, 1, ht);
	} else if (ht) {
		ht = NULL;
	}

	return ht;
}

static php_resource_factory_ops_t php_persistent_handle_resource_factory_ops = {
	(php_resource_factory_handle_ctor_t) php_persistent_handle_acquire,
	(php_resource_factory_handle_copy_t) php_persistent_handle_accrete,
	(php_resource_factory_handle_dtor_t) php_persistent_handle_release
};

php_resource_factory_ops_t *php_persistent_handle_get_resource_factory_ops(void)
{
	return &php_persistent_handle_resource_factory_ops;
}

ZEND_BEGIN_ARG_INFO_EX(ai_raphf_stat_persistent_handles, 0, 0, 0)
ZEND_END_ARG_INFO();
static PHP_FUNCTION(raphf_stat_persistent_handles)
{
	if (SUCCESS == zend_parse_parameters_none()) {
		object_init(return_value);
		if (php_persistent_handle_statall(HASH_OF(return_value) TSRMLS_CC)) {
			return;
		}
		zval_dtor(return_value);
	}
	RETURN_FALSE;
}

ZEND_BEGIN_ARG_INFO_EX(ai_raphf_clean_persistent_handles, 0, 0, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, ident)
ZEND_END_ARG_INFO();
static PHP_FUNCTION(raphf_clean_persistent_handles)
{
	char *name_str = NULL, *ident_str = NULL;
	int name_len = 0, ident_len = 0;

	if (SUCCESS == zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!s!",
			&name_str, &name_len, &ident_str, &ident_len)) {
		php_persistent_handle_cleanup(name_str, name_len, ident_str,
				ident_len TSRMLS_CC);
	}
}

#if PHP_RAPHF_TEST
#	include "php_raphf_test.c"
#endif

static const zend_function_entry raphf_functions[] = {
	ZEND_NS_FENTRY("raphf", stat_persistent_handles,
			ZEND_FN(raphf_stat_persistent_handles),
			ai_raphf_stat_persistent_handles, 0)
	ZEND_NS_FENTRY("raphf", clean_persistent_handles,
			ZEND_FN(raphf_clean_persistent_handles),
			ai_raphf_clean_persistent_handles, 0)
#if PHP_RAPHF_TEST
	ZEND_NS_FENTRY("raphf", provide, ZEND_FN(raphf_provide), NULL, 0)
	ZEND_NS_FENTRY("raphf", conceal, ZEND_FN(raphf_conceal), NULL, 0)
	ZEND_NS_FENTRY("raphf", concede, ZEND_FN(raphf_concede), NULL, 0)
	ZEND_NS_FENTRY("raphf", dispute, ZEND_FN(raphf_dispute), NULL, 0)
	ZEND_NS_FENTRY("raphf", handle_ctor, ZEND_FN(raphf_handle_ctor), NULL, 0)
	ZEND_NS_FENTRY("raphf", handle_copy, ZEND_FN(raphf_handle_copy), NULL, 0)
	ZEND_NS_FENTRY("raphf", handle_dtor, ZEND_FN(raphf_handle_dtor), NULL, 0)
#endif
	{0}
};

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("raphf.persistent_handle.limit", "-1", PHP_INI_SYSTEM,
			OnUpdateLong, persistent_handle.limit, zend_raphf_globals,
			raphf_globals)
PHP_INI_END()

static HashTable *php_persistent_handles_global_hash;

static PHP_GINIT_FUNCTION(raphf)
{
	raphf_globals->persistent_handle.limit = -1;

	zend_hash_init(&raphf_globals->persistent_handle.hash, 0, NULL,
			php_persistent_handle_hash_dtor, 1);
	if (php_persistent_handles_global_hash) {
		zend_hash_copy(&raphf_globals->persistent_handle.hash,
				php_persistent_handles_global_hash, NULL);
	}
}

static PHP_GSHUTDOWN_FUNCTION(raphf)
{
	zend_hash_destroy(&raphf_globals->persistent_handle.hash);
}

PHP_MINIT_FUNCTION(raphf)
{
	php_persistent_handles_global_hash = &PHP_RAPHF_G->persistent_handle.hash;

#if PHP_RAPHF_TEST
	PHP_MINIT(raphf_test)(INIT_FUNC_ARGS_PASSTHRU);
#endif

	REGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(raphf)
{
#if PHP_RAPHF_TEST
	PHP_MSHUTDOWN(raphf_test)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
#endif

	UNREGISTER_INI_ENTRIES();
	php_persistent_handles_global_hash = NULL;
	return SUCCESS;
}

static int php_persistent_handle_apply_info_ex(zval *p TSRMLS_DC, int argc,
		va_list argv, zend_hash_key *key)
{
	php_persistent_handle_list_t *list = Z_PTR_P(p);
	zend_hash_key *super_key = va_arg(argv, zend_hash_key *);
	char used[21], free[21];

	slprintf(used, sizeof(used), "%u", list->used);
	slprintf(free, sizeof(free), "%d", zend_hash_num_elements(&list->free));

	php_info_print_table_row(4, super_key->key->val, key->key->val, used, free);

	return ZEND_HASH_APPLY_KEEP;
}

static int php_persistent_handle_apply_info(zval *p TSRMLS_DC, int argc,
		va_list argv, zend_hash_key *key)
{
	php_persistent_handle_provider_t *provider = Z_PTR_P(p);

	zend_hash_apply_with_arguments(&provider->list.free TSRMLS_CC,
			php_persistent_handle_apply_info_ex, 1, key);

	return ZEND_HASH_APPLY_KEEP;
}

PHP_MINFO_FUNCTION(raphf)
{
	php_info_print_table_start();
	php_info_print_table_header(2,
			"Resource and persistent handle factory support", "enabled");
	php_info_print_table_row(2, "Extension version", PHP_RAPHF_VERSION);
	php_info_print_table_end();

	php_info_print_table_start();
	php_info_print_table_colspan_header(4, "Persistent handles in this "
#ifdef ZTS
			"thread"
#else
			"process"
#endif
	);
	php_info_print_table_header(4, "Provider", "Ident", "Used", "Free");
	zend_hash_apply_with_arguments(
			&PHP_RAPHF_G->persistent_handle.hash TSRMLS_CC,
			php_persistent_handle_apply_info, 0);
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

zend_module_entry raphf_module_entry = {
	STANDARD_MODULE_HEADER,
	"raphf",
	raphf_functions,
	PHP_MINIT(raphf),
	PHP_MSHUTDOWN(raphf),
	NULL,
	NULL,
	PHP_MINFO(raphf),
	PHP_RAPHF_VERSION,
	ZEND_MODULE_GLOBALS(raphf),
	PHP_GINIT(raphf),
	PHP_GSHUTDOWN(raphf),
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_RAPHF
ZEND_GET_MODULE(raphf)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
