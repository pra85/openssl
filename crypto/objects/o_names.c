#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/lhash.h>
#include <openssl/objects.h>
#include <openssl/safestack.h>
#include <openssl/e_os2.h>
#include "obj_lcl.h"

/*
 * Later versions of DEC C has started to add lnkage information to certain
 * functions, which makes it tricky to use them as values to regular function
 * pointers.  One way is to define a macro that takes care of casting them
 * correctly.
 */
#ifdef OPENSSL_SYS_VMS_DECC
# define OPENSSL_strcmp (int (*)(const char *,const char *))strcmp
#else
# define OPENSSL_strcmp strcmp
#endif

/*
 * I use the ex_data stuff to manage the identifiers for the obj_name_types
 * that applications may define.  I only really use the free function field.
 */
static LHASH_OF(OBJ_NAME) *names_lh = NULL;
static int names_type_num = OBJ_NAME_TYPE_NUM;

struct name_funcs_st {
    unsigned long (*hash_func) (const char *name);
    int (*cmp_func) (const char *a, const char *b);
    void (*free_func) (const char *, int, const char *);
};

static STACK_OF(NAME_FUNCS) *name_funcs_stack;

/*
 * The LHASH callbacks now use the raw "void *" prototypes and do
 * per-variable casting in the functions. This prevents function pointer
 * casting without the need for macro-generated wrapper functions.
 */

static unsigned long obj_name_hash(const OBJ_NAME *a);
static int obj_name_cmp(const OBJ_NAME *a, const OBJ_NAME *b);

int OBJ_NAME_init(void)
{
    if (names_lh != NULL)
        return (1);
    CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_DISABLE);
    names_lh = lh_OBJ_NAME_new(obj_name_hash, obj_name_cmp);
    CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ENABLE);
    return (names_lh != NULL);
}

int OBJ_NAME_new_index(unsigned long (*hash_func) (const char *),
                       int (*cmp_func) (const char *, const char *),
                       void (*free_func) (const char *, int, const char *))
{
    int ret;
    int i;
    NAME_FUNCS *name_funcs;

    if (name_funcs_stack == NULL) {
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_DISABLE);
        name_funcs_stack = sk_NAME_FUNCS_new_null();
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ENABLE);
    }
    if (name_funcs_stack == NULL) {
        /* ERROR */
        return (0);
    }
    ret = names_type_num;
    names_type_num++;
    for (i = sk_NAME_FUNCS_num(name_funcs_stack); i < names_type_num; i++) {
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_DISABLE);
        name_funcs = OPENSSL_zalloc(sizeof(*name_funcs));
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ENABLE);
        if (name_funcs == NULL) {
            OBJerr(OBJ_F_OBJ_NAME_NEW_INDEX, ERR_R_MALLOC_FAILURE);
            return (0);
        }
        name_funcs->hash_func = lh_strhash;
        name_funcs->cmp_func = OPENSSL_strcmp;
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_DISABLE);
        sk_NAME_FUNCS_push(name_funcs_stack, name_funcs);
        CRYPTO_mem_ctrl(CRYPTO_MEM_CHECK_ENABLE);
    }
    name_funcs = sk_NAME_FUNCS_value(name_funcs_stack, ret);
    if (hash_func != NULL)
        name_funcs->hash_func = hash_func;
    if (cmp_func != NULL)
        name_funcs->cmp_func = cmp_func;
    if (free_func != NULL)
        name_funcs->free_func = free_func;
    return (ret);
}

static int obj_name_cmp(const OBJ_NAME *a, const OBJ_NAME *b)
{
    int ret;

    ret = a->type - b->type;
    if (ret == 0) {
        if ((name_funcs_stack != NULL)
            && (sk_NAME_FUNCS_num(name_funcs_stack) > a->type)) {
            ret = sk_NAME_FUNCS_value(name_funcs_stack,
                                      a->type)->cmp_func(a->name, b->name);
        } else
            ret = strcmp(a->name, b->name);
    }
    return (ret);
}

static unsigned long obj_name_hash(const OBJ_NAME *a)
{
    unsigned long ret;

    if ((name_funcs_stack != NULL)
        && (sk_NAME_FUNCS_num(name_funcs_stack) > a->type)) {
        ret =
            sk_NAME_FUNCS_value(name_funcs_stack,
                                a->type)->hash_func(a->name);
    } else {
        ret = lh_strhash(a->name);
    }
    ret ^= a->type;
    return (ret);
}

const char *OBJ_NAME_get(const char *name, int type)
{
    OBJ_NAME on, *ret;
    int num = 0, alias;

    if (name == NULL)
        return (NULL);
    if ((names_lh == NULL) && !OBJ_NAME_init())
        return (NULL);

    alias = type & OBJ_NAME_ALIAS;
    type &= ~OBJ_NAME_ALIAS;

    on.name = name;
    on.type = type;

    for (;;) {
        ret = lh_OBJ_NAME_retrieve(names_lh, &on);
        if (ret == NULL)
            return (NULL);
        if ((ret->alias) && !alias) {
            if (++num > 10)
                return (NULL);
            on.name = ret->data;
        } else {
            return (ret->data);
        }
    }
}

int OBJ_NAME_add(const char *name, int type, const char *data)
{
    OBJ_NAME *onp, *ret;
    int alias;

    if ((names_lh == NULL) && !OBJ_NAME_init())
        return (0);

    alias = type & OBJ_NAME_ALIAS;
    type &= ~OBJ_NAME_ALIAS;

    onp = OPENSSL_malloc(sizeof(*onp));
    if (onp == NULL) {
        /* ERROR */
        return (0);
    }

    onp->name = name;
    onp->alias = alias;
    onp->type = type;
    onp->data = data;

    ret = lh_OBJ_NAME_insert(names_lh, onp);
    if (ret != NULL) {
        /* free things */
        if ((name_funcs_stack != NULL)
            && (sk_NAME_FUNCS_num(name_funcs_stack) > ret->type)) {
            /*
             * XXX: I'm not sure I understand why the free function should
             * get three arguments... -- Richard Levitte
             */
            sk_NAME_FUNCS_value(name_funcs_stack,
                                ret->type)->free_func(ret->name, ret->type,
                                                      ret->data);
        }
        OPENSSL_free(ret);
    } else {
        if (lh_OBJ_NAME_error(names_lh)) {
            /* ERROR */
            return (0);
        }
    }
    return (1);
}

int OBJ_NAME_remove(const char *name, int type)
{
    OBJ_NAME on, *ret;

    if (names_lh == NULL)
        return (0);

    type &= ~OBJ_NAME_ALIAS;
    on.name = name;
    on.type = type;
    ret = lh_OBJ_NAME_delete(names_lh, &on);
    if (ret != NULL) {
        /* free things */
        if ((name_funcs_stack != NULL)
            && (sk_NAME_FUNCS_num(name_funcs_stack) > ret->type)) {
            /*
             * XXX: I'm not sure I understand why the free function should
             * get three arguments... -- Richard Levitte
             */
            sk_NAME_FUNCS_value(name_funcs_stack,
                                ret->type)->free_func(ret->name, ret->type,
                                                      ret->data);
        }
        OPENSSL_free(ret);
        return (1);
    } else
        return (0);
}

typedef struct {
    int type;
    void (*fn) (const OBJ_NAME *, void *arg);
    void *arg;
} OBJ_DOALL;

static void do_all_fn(const OBJ_NAME *name, OBJ_DOALL *d)
{
    if (name->type == d->type)
        d->fn(name, d->arg);
}

IMPLEMENT_LHASH_DOALL_ARG_CONST(OBJ_NAME, OBJ_DOALL);

void OBJ_NAME_do_all(int type, void (*fn) (const OBJ_NAME *, void *arg),
                     void *arg)
{
    OBJ_DOALL d;

    d.type = type;
    d.fn = fn;
    d.arg = arg;

    lh_OBJ_NAME_doall_OBJ_DOALL(names_lh, do_all_fn, &d);
}

struct doall_sorted {
    int type;
    int n;
    const OBJ_NAME **names;
};

static void do_all_sorted_fn(const OBJ_NAME *name, void *d_)
{
    struct doall_sorted *d = d_;

    if (name->type != d->type)
        return;

    d->names[d->n++] = name;
}

static int do_all_sorted_cmp(const void *n1_, const void *n2_)
{
    const OBJ_NAME *const *n1 = n1_;
    const OBJ_NAME *const *n2 = n2_;

    return strcmp((*n1)->name, (*n2)->name);
}

void OBJ_NAME_do_all_sorted(int type,
                            void (*fn) (const OBJ_NAME *, void *arg),
                            void *arg)
{
    struct doall_sorted d;
    int n;

    d.type = type;
    d.names =
        OPENSSL_malloc(sizeof(*d.names) * lh_OBJ_NAME_num_items(names_lh));
    /* Really should return an error if !d.names...but its a void function! */
    if (d.names != NULL) {
        d.n = 0;
        OBJ_NAME_do_all(type, do_all_sorted_fn, &d);

        qsort((void *)d.names, d.n, sizeof(*d.names), do_all_sorted_cmp);

        for (n = 0; n < d.n; ++n)
            fn(d.names[n], arg);

        OPENSSL_free((void *)d.names);
    }
}

static int free_type;

static void names_lh_free_doall(OBJ_NAME *onp)
{
    if (onp == NULL)
        return;

    if (free_type < 0 || free_type == onp->type)
        OBJ_NAME_remove(onp->name, onp->type);
}

static void name_funcs_free(NAME_FUNCS *ptr)
{
    OPENSSL_free(ptr);
}

void OBJ_NAME_cleanup(int type)
{
    unsigned long down_load;

    if (names_lh == NULL)
        return;

    free_type = type;
    down_load = lh_OBJ_NAME_get_down_load(names_lh);
    lh_OBJ_NAME_set_down_load(names_lh, 0);

    lh_OBJ_NAME_doall(names_lh, names_lh_free_doall);
    if (type < 0) {
        lh_OBJ_NAME_free(names_lh);
        sk_NAME_FUNCS_pop_free(name_funcs_stack, name_funcs_free);
        names_lh = NULL;
        name_funcs_stack = NULL;
    } else
        lh_OBJ_NAME_set_down_load(names_lh, down_load);
}
