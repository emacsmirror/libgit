#include <stdlib.h>

#include "emacs-module.h"
#include "git2.h"
#include "uthash.h"

#include "interface.h"
#include "egit-clone.h"
#include "egit-object.h"
#include "egit-reference.h"
#include "egit-repository.h"
#include "egit-revparse.h"
#include "egit.h"

// Hash table of stored objects
egit_object *object_store = NULL;

egit_type egit_get_type(emacs_env *env, emacs_value _obj)
{
    if (!em_user_ptrp(env, _obj))
        return EGIT_UNKNOWN;
    egit_object *obj = (egit_object*)env->get_user_ptr(env, _obj);
    return obj->type;
}

bool egit_assert_type(emacs_env *env, emacs_value obj, egit_type type, emacs_value predicate)
{
    if (type == egit_get_type(env, obj))
        return true;
    em_signal_wrong_type(env, predicate, obj);
    return false;
}

bool egit_assert_object(emacs_env *env, emacs_value obj)
{
    egit_type type = egit_get_type(env, obj);
    if (type == EGIT_COMMIT || type == EGIT_TREE ||
        type == EGIT_BLOB || type == EGIT_TAG || type == EGIT_OBJECT)
        return true;
    em_signal_wrong_type(env, em_git_object_p, obj);
    return false;
}

void egit_decref_wrapped(void *obj)
{
    // Look up the wrapper struct in the hash table, and call egit_decref_wrapper
    egit_object *wrapper;
    HASH_FIND_PTR(object_store, &obj, wrapper);
    egit_decref_wrapper(wrapper);
}

void egit_decref_wrapper(void *_obj)
{
    // The argument type must be void* to make this function work as an Emacs finalizer
    egit_object *obj = (egit_object*)_obj;
    obj->refcount--;

    if (obj->refcount != 0)
        return;

    // First, delete the wrapper from the object store
    HASH_DEL(object_store, obj);

    // Decref any owner objects if applicable, and free the libgit2 struct
    // Note that this object must be freed before calling decref on others
    switch (obj->type) {
    case EGIT_COMMIT:
    case EGIT_TREE:
    case EGIT_BLOB:
    case EGIT_TAG:
    case EGIT_OBJECT: {
        git_repository *repo = git_object_owner(obj->ptr);
        git_object_free(obj->ptr);
        egit_decref_wrapped(repo);
        break;
    }
    case EGIT_REFERENCE: {
        git_repository *repo = git_reference_owner(obj->ptr);
        git_reference_free(obj->ptr);
        egit_decref_wrapped(repo);
        break;
    }
    case EGIT_REPOSITORY:
        git_repository_free(obj->ptr);
        break;
    case EGIT_UNKNOWN:
        break;
    }

    // Finally, free the wrapper struct
    free(obj);
}

/**
 * Increase the reference count of the given pointer.
 * If the pointer does not exist in the object store, add it with a refcount of one.
 * Otherwise, increase the refcount by one.
 * @param type The type of the libgit2 structure to store.
 * @param obj The pointer to store.
 * @return Pointer to the egit_object wrapper struct.
 */
static egit_object *egit_incref(egit_type type, void *obj)
{
    egit_object *retval;
    HASH_FIND_PTR(object_store, &obj, retval);

    if (retval)
        // Object is already stored, just incref
        retval->refcount++;

    else {
        // Object must be added
        retval = (egit_object*)malloc(sizeof(egit_object));
        retval->type = type;
        retval->refcount = 1;
        retval->ptr = obj;
        HASH_ADD_PTR(object_store, ptr, retval);
    }

    return retval;
}

emacs_value egit_wrap(emacs_env* env, egit_type type, void* data)
{
    // If it's a git_object, try to be more specific
    if (type == EGIT_OBJECT) {
        switch (git_object_type(data)) {
        case GIT_OBJ_COMMIT: type = EGIT_COMMIT; break;
        case GIT_OBJ_TREE: type = EGIT_TREE; break;
        case GIT_OBJ_BLOB: type = EGIT_BLOB; break;
        case GIT_OBJ_TAG: type = EGIT_TAG; break;
        default: break;
        }
    }

    // Ensure that the object is added to the store, with a reference
    egit_object *obj = egit_incref(type, data);

    // Increase refcounts of owner object(s), if applicable
    switch (type) {
    case EGIT_COMMIT:
    case EGIT_TREE:
    case EGIT_BLOB:
    case EGIT_TAG:
    case EGIT_OBJECT:
        egit_incref(EGIT_REPOSITORY, git_object_owner(data));
        break;
    case EGIT_REFERENCE:
        egit_incref(EGIT_REPOSITORY, git_reference_owner(data));
        break;
    default:
        break;
    }

    // Make an Emacs user pointer to the wrapper, and return
    return env->make_user_ptr(env, egit_decref_wrapper, obj);
}

typedef emacs_value (*func_1)(emacs_env*, emacs_value);
typedef emacs_value (*func_2)(emacs_env*, emacs_value, emacs_value);

// Get an argument index, or nil. Useful for simulating optional arguments.
#define GET_SAFE(arglist, nargs, index) ((index) < (nargs) ? (arglist)[(index)] : em_nil)

emacs_value egit_dispatch_1(emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
    func_1 func = (func_1) data;
    return func(env, GET_SAFE(args, nargs, 0));
}

emacs_value egit_dispatch_2(emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
    func_2 func = (func_2) data;
    return func(env, GET_SAFE(args, nargs, 0), GET_SAFE(args, nargs, 1));
}

bool egit_dispatch_error(emacs_env *env, int retval)
{
    if (retval >= 0) return false;

    const git_error *err = giterr_last();
    if (!err) return false;

    em_signal_giterr(env, err->klass, err->message);
    return true;
}

#define DEFUN(ename, cname, min_nargs, max_nargs)                       \
    em_defun(env, (ename),                                              \
             env->make_function(                                        \
                 env, (min_nargs), (max_nargs),                         \
                 egit_dispatch_##max_nargs,                             \
                 egit_##cname##__doc,                                   \
                 egit_##cname))

void egit_init(emacs_env *env)
{
    // Clone
    DEFUN("git-clone", clone, 2, 2);

    // Object
    DEFUN("git-object-id", object_id, 1, 1);
    DEFUN("git-object-short-id", object_short_id, 1, 1);

    DEFUN("git-object-p", object_p, 1, 1);

    // Reference
    DEFUN("git-reference-name", reference_name, 1, 1);
    DEFUN("git-reference-owner", reference_owner, 1, 1);
    DEFUN("git-reference-resolve", reference_resolve, 1, 1);
    DEFUN("git-reference-target", reference_target, 1, 1);

    DEFUN("git-reference-p", reference_p, 1, 1);

    // Repository
    DEFUN("git-repository-init", repository_init, 1, 2);
    DEFUN("git-repository-open", repository_open, 1, 1);
    DEFUN("git-repository-open-bare", repository_open_bare, 1, 1);

    DEFUN("git-repository-commondir", repository_commondir, 1, 1);
    DEFUN("git-repository-get-namespace", repository_get_namespace, 1, 1);
    DEFUN("git-repository-head", repository_head, 1, 1);
    DEFUN("git-repository-head-for-worktree", repository_head_for_worktree, 2, 2);
    DEFUN("git-repository-ident", repository_ident, 1, 1);
    DEFUN("git-repository-message", repository_message, 1, 1);
    DEFUN("git-repository-path", repository_path, 1, 1);
    DEFUN("git-repository-state", repository_state, 1, 1);
    DEFUN("git-repository-workdir", repository_workdir, 1, 1);

    DEFUN("git-repository-detach-head", repository_detach_head, 1, 1);
    DEFUN("git-repository-message-remove", repository_message_remove, 1, 1);

    DEFUN("git-repository-p", repository_p, 1, 1);
    DEFUN("git-repository-bare-p", repository_bare_p, 1, 1);
    DEFUN("git-repository-empty-p", repository_empty_p, 1, 1);
    DEFUN("git-repository-head-detached-p", repository_empty_p, 1, 1);
    DEFUN("git-repository-head-unborn-p", repository_empty_p, 1, 1);
    DEFUN("git-repository-shallow-p", repository_shallow_p, 1, 1);
    DEFUN("git-repository-worktree-p", repository_worktree_p, 1, 1);

    // Revparse
    DEFUN("git-revparse-single", revparse_single, 2, 2);
}
