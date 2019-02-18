#include "duktape_binding_helper.h"
#include "../ecmascript.h"
#include "../ecmascript_instance.h"
#include "../ecmascript_language.h"
#include "core/engine.h"
#include "core/global_constants.h"

Object *DuktapeBindingHelper::ecma_instance_target = NULL;

void DuktapeBindingHelper::fatal_function(void *udata, const char *msg) {
	fprintf(stderr, "*** FATAL ERROR: %s\n", (msg ? msg : "no message"));
	fflush(stderr);
#ifdef DEBUG_ENABLED
	CRASH_NOW();
#elif
	abort();
#endif
}

void DuktapeBindingHelper::set_weak_ref(ObjectID p_id, DuktapeHeapObject *ptr) {
	weakref_pool.set(p_id, ptr);
}

DuktapeHeapObject *DuktapeBindingHelper::get_weak_ref(Object *obj) {
	ERR_FAIL_NULL_V(obj, NULL);
	void **ele_ptr = get_singleton()->weakref_pool.getptr(obj->get_instance_id());
	if (ele_ptr) {
		return *ele_ptr;
	}
	return NULL;
}

void DuktapeBindingHelper::set_strong_ref(ObjectID p_id, DuktapeHeapObject *ptr) {

	weakref_pool.set(p_id, ptr);
	strongref_pool.set(p_id, ptr);

	// fill the script instance binding data to make sure godot_refcount_decremented can get when unreference
	if (Object *obj = ObjectDB::get_instance(p_id)) {
		obj->get_script_instance_binding(get_language()->get_language_index());
	}

	duk_push_strong_ref_container(ctx);
	duk_push_heapptr(ctx, ptr);
	duk_put_prop_index(ctx, -2, p_id);
	duk_pop(ctx);
}

DuktapeHeapObject *DuktapeBindingHelper::get_strong_ref(Object *obj) {
	DuktapeHeapObject **val_ptr = strongref_pool.getptr(obj->get_instance_id());
	if (val_ptr) {
		return *val_ptr;
	}
	return NULL;
}

void DuktapeBindingHelper::duk_push_strong_ref_container(duk_context *ctx) {
	duk_push_heapptr(ctx, strongref_pool_ptr);
}

duk_ret_t DuktapeBindingHelper::duk_godot_object_constructor(duk_context *ctx) {

	duk_push_current_function(ctx);
	duk_get_prop_literal(ctx, -1, PROTOTYPE_LITERAL);
	duk_get_prop_literal(ctx, -1, DUK_HIDDEN_SYMBOL("cls"));
	ClassDB::ClassInfo *cls = static_cast<ClassDB::ClassInfo *>(duk_get_pointer_default(ctx, -1, NULL));
	duk_pop_3(ctx);

	Object *src_obj = ecma_instance_target;
	ecma_instance_target = NULL;

	ERR_FAIL_NULL_V(cls, DUK_ERR_TYPE_ERROR);
	ERR_FAIL_NULL_V(cls->creation_func, DUK_ERR_TYPE_ERROR);

	Object *obj = src_obj == NULL ? cls->creation_func() : src_obj;
	duk_push_godot_object(ctx, obj, true);
	ERR_FAIL_COND_V(duk_is_null_or_undefined(ctx, -1), DUK_ERR_TYPE_ERROR);

	// create script instance for this object
	if (src_obj == NULL) {
		duk_get_prop_literal(ctx, -1, ECMA_CLASS_NAME_LITERAL);
		const char *ecma_class_name = duk_get_string_default(ctx, -1, NULL);
		duk_pop(ctx);
		if (ecma_class_name) {
			if (ECMAClassInfo *ecma_class = get_singleton()->ecma_classes.getptr(ecma_class_name)) {
				ECMAScriptInstance *inst = memnew(ECMAScriptInstance);
				inst->owner = obj;
				inst->ecma_object = { duk_get_heapptr(ctx, -1) };
				inst->script = get_language()->script_classes.get(ecma_class_name);
				obj->set_script_instance(inst);
			}
		}
	}

	return DUK_NO_RET_VAL;
}

duk_ret_t DuktapeBindingHelper::duk_godot_object_finalizer(duk_context *ctx) {
	Object *ptr = duk_get_godot_object(ctx, -1);
	if (NULL == ptr) return DUK_NO_RET_VAL;
	if (Reference *ref = Object::cast_to<Reference>(ptr)) {
		if (ref->unreference()) {
			// A reference without C++ usage
			get_singleton()->weakref_pool.erase(ptr->get_instance_id());
			memdelete(ref);
		} else if (ref->get_script_instance_binding(get_language()->get_language_index())) {
			// A reference not used in C++
			get_singleton()->weakref_pool.erase(ptr->get_instance_id());
			get_singleton()->strongref_pool.erase(ptr->get_instance_id());
			memdelete(ref);
		} else {
			// A reference with other C++ reference
			// Rescue the ecmascript object as the reference is still alive
			get_singleton()->set_strong_ref(ref->get_instance_id(), duk_get_heapptr(ctx, 0));
		}
	}
	return DUK_NO_RET_VAL;
}

void DuktapeBindingHelper::godot_refcount_incremented(Reference *p_object) {
}

bool DuktapeBindingHelper::godot_refcount_decremented(Reference *p_object) {
	int refcount = p_object->reference_get_count();
	ECMAScriptBindingData *gc_handler = static_cast<ECMAScriptBindingData *>(p_object->get_script_instance_binding(get_language()->get_language_index()));
	if (gc_handler) {
		if (refcount == 0) {
			// clear taged strong reference in script
			// the life of this refernce is given to script gc
			set_strong_ref(gc_handler->instance_id, NULL);
		}
		return false;
	}
	return refcount == 0;
}

Error DuktapeBindingHelper::eval_string(const String &p_source) {
	ERR_FAIL_NULL_V(ctx, ERR_SKIP);
	duk_eval_string(ctx, p_source.utf8().ptr());
	return OK;
}

void *DuktapeBindingHelper::alloc_object_binding_data(Object *p_object) {
	ECMAScriptBindingData *handler = NULL;
	if (DuktapeHeapObject *heap_ptr = get_strong_ref(p_object)) {
		handler = memnew(ECMAScriptBindingData);
		handler->godot_object = p_object;
		handler->instance_id = p_object->get_instance_id();
		handler->ecma_object = heap_ptr;
	}
	return handler;
}

void DuktapeBindingHelper::free_object_binding_data(void *p_gc_handler) {
	if (ECMAScriptBindingData *handler = static_cast<ECMAScriptBindingData *>(p_gc_handler)) {
		if (Object::cast_to<Reference>(handler->godot_object)) {
			// References don't need do this as they are weak referenced or they
			return;
		} else if (handler->ecma_object) {
			// clear taged script reference
			set_strong_ref(handler->instance_id, NULL);
		}
		memdelete(handler);
	}
}

duk_ret_t DuktapeBindingHelper::godot_object_free(duk_context *ctx) {
	duk_push_this(ctx);
	Object *obj = duk_get_godot_object(ctx, -1);
	ERR_FAIL_NULL_V(obj, DUK_NO_RET_VAL);
	if (Object::cast_to<Reference>(obj)) {
		ERR_FAIL_V(DUK_ERR_TYPE_ERROR);
	} else {
		memdelete(obj);
		duk_del_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("ptr"));
	}
	return DUK_NO_RET_VAL;
}

duk_ret_t DuktapeBindingHelper::godot_print_function(duk_context *ctx) {
	int size = duk_get_top(ctx);
	String msg;
	for (int i = 0; i < size; ++i) {
		msg += DuktapeBindingHelper::duk_get_godot_string(ctx, i, true);
		if (i < size - 1) {
			msg += " ";
		}
	}
	print_line(msg);
	return DUK_NO_RET_VAL;
}

duk_ret_t DuktapeBindingHelper::duk_godot_object_method(duk_context *ctx) {

	duk_idx_t argc = duk_get_top(ctx);

	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, DUK_HIDDEN_SYMBOL("mb"));
	MethodBind *mb = static_cast<MethodBind *>(duk_get_pointer_default(ctx, -1, NULL));
	ERR_FAIL_NULL_V(mb, DUK_NO_RET_VAL);

	duk_push_this(ctx);
	Object *ptr = duk_get_godot_object(ctx, -1);
	ERR_FAIL_NULL_V(ptr, DUK_NO_RET_VAL);

	Variant::CallError err;

	argc = MIN(argc, mb->get_argument_count());

	const Variant **args = memnew_arr(const Variant *, argc);
	Vector<Variant> vargs;
	vargs.resize(argc);
	for (duk_idx_t i = 0; i < argc; ++i) {
		vargs.write[i] = duk_get_godot_variant(ctx, i);
		args[i] = (vargs.ptr() + i);
	}
	const Variant &ret_val = mb->call(ptr, args, argc, err);
	memdelete_arr(args);

#ifdef DEBUG_METHODS_ENABLED
	ERR_FAIL_COND_V(err.error != Variant::CallError::CALL_OK, DUK_ERR_TYPE_ERROR);
#endif

	if (mb->has_return()) {
		duk_push_godot_variant(ctx, ret_val);
		return DUK_HAS_RET_VAL;
	}
	return DUK_NO_RET_VAL;
}

duk_ret_t DuktapeBindingHelper::godot_object_to_string(duk_context *ctx) {
	duk_push_this(ctx);
	Variant var = duk_get_godot_object(ctx, -1);
	duk_push_godot_string(ctx, var);
	return DUK_HAS_RET_VAL;
}

void DuktapeBindingHelper::duk_push_godot_variant(duk_context *ctx, const Variant &var) {
	switch (var.get_type()) {
		case Variant::NIL:
			duk_push_null(ctx);
			break;
		case Variant::BOOL:
			duk_push_boolean(ctx, duk_bool_t((bool)var));
			break;
		case Variant::REAL:
		case Variant::INT:
			duk_push_number(ctx, var);
			break;
		case Variant::STRING: {
			duk_push_godot_string(ctx, var);
		} break;
		case Variant::OBJECT:
			duk_push_godot_object(ctx, var);
			break;
		default:
			// not supported
			duk_push_undefined(ctx);
			break;
	}
}

Variant DuktapeBindingHelper::duk_get_godot_variant(duk_context *ctx, duk_idx_t idx) {
	duk_int_t type = duk_get_type(ctx, idx);
	switch (type) {
		case DUK_TYPE_BOOLEAN:
			return Variant(duk_get_boolean(ctx, idx) == true);
		case DUK_TYPE_NUMBER:
			return Variant(duk_get_number(ctx, idx));
		case DUK_TYPE_STRING: {
			String str;
			str.parse_utf8(duk_get_string(ctx, idx));
			return str;
		} break;
		case DUK_TYPE_OBJECT: {
			Object *obj = duk_get_godot_object(ctx, idx);
			if (Reference *r = Object::cast_to<Reference>(obj)) {
				// Add reference count as the REF construct from Reference* don't increase the reference count
				// This reference count will minused in the desctuctor of REF
				REF ref(r);
				ref->reference();
				return ref;
			}
			return obj;
		}
		case DUK_TYPE_NULL:
		case DUK_TYPE_UNDEFINED:
		default:
			return Variant();
	}
}

String DuktapeBindingHelper::duk_get_godot_string(duk_context *ctx, duk_idx_t idx, bool convert_type) {
	const char *raw_str = convert_type ? duk_to_string(ctx, idx) : duk_get_string(ctx, idx);
	String str;
	str.parse_utf8(raw_str);
	return str;
}

Object *DuktapeBindingHelper::duk_get_godot_object(duk_context *ctx, duk_idx_t idx) {
	duk_get_prop_string(ctx, idx, DUK_HIDDEN_SYMBOL("ptr"));
	return static_cast<Object *>(duk_get_pointer_default(ctx, -1, NULL));
}

void DuktapeBindingHelper::duk_push_godot_object(duk_context *ctx, Object *obj, bool from_constructor) {
	if (obj) {
		DuktapeHeapObject *heap_obj = get_singleton()->get_weak_ref(obj);
		if (heap_obj) {
			duk_push_heapptr(ctx, heap_obj);
		} else {
			if (from_constructor) {
				duk_push_this(ctx);
			} else {
				duk_push_object(ctx);
				duk_push_heapptr(ctx, get_singleton()->native_class_prototypes.get(obj->get_class_name()));
				duk_put_prop_literal(ctx, -2, PROTO_LITERAL);
			}

			duk_push_pointer(ctx, obj);
			duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("ptr"));

			heap_obj = duk_get_heapptr(ctx, -1);
			if (Reference *ref = Object::cast_to<Reference>(obj)) {
				get_singleton()->set_weak_ref(ref->get_instance_id(), heap_obj);
			} else {
				// The strong reference is released when object is going to die
				// See godot_free_instance_callback and godot_refcount_decremented
				get_singleton()->set_strong_ref(obj->get_instance_id(), heap_obj);
			}
		}
	} else {
		duk_push_undefined(ctx);
	}
}

void DuktapeBindingHelper::duk_push_godot_string(duk_context *ctx, const String &str) {
	if (str.empty()) {
		duk_push_lstring(ctx, NULL, 0);
	} else {
		duk_push_string(ctx, str.utf8().ptr());
	}
}

void DuktapeBindingHelper::duk_push_godot_string_name(duk_context *ctx, const StringName &str) {
	duk_push_godot_string(ctx, str);
}

void DuktapeBindingHelper::duk_put_prop_godot_string(duk_context *ctx, duk_idx_t idx, const String &str) {
	duk_put_prop_string(ctx, idx, str.utf8().ptr());
}

void DuktapeBindingHelper::duk_put_prop_godot_string_name(duk_context *ctx, duk_idx_t idx, const StringName &str) {
	duk_put_prop_godot_string(ctx, idx, str);
}

void DuktapeBindingHelper::rigister_class(duk_context *ctx, const ClassDB::ClassInfo *cls) {
	duk_require_stack(ctx, 2);

	// Class constuctor function
	duk_push_c_function(ctx, duk_godot_object_constructor, 0);
	{
		// constants
		for (const StringName* const_key = cls->constant_map.next(NULL); const_key; const_key = cls->constant_map.next(const_key)) {
			duk_push_godot_variant(ctx, cls->constant_map.get(*const_key));
			String name = *const_key;
			duk_put_prop_string(ctx, -2, name.utf8().ptr());
		}
		// enumrations
		for (const StringName* enum_key = cls->enum_map.next(NULL); enum_key; enum_key = cls->enum_map.next(enum_key)) {
			const List<StringName> & consts = cls->enum_map.get(*enum_key);
			duk_push_object(ctx);
			for (const List<StringName>::Element * E = consts.front(); E; E = E->next()) {
				duk_push_godot_variant(ctx, cls->constant_map.get(E->get()));
				String const_name = E->get();
				duk_put_prop_string(ctx, -2, const_name.utf8().ptr());
			}
			String enum_name = *enum_key;
			duk_put_prop_string(ctx, -2, enum_name.utf8().ptr());
		}
		// Class.prototype
		duk_push_object(ctx);
		// Class.prototype.cls
		duk_push_pointer(ctx, (void *)cls);
		duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("cls"));
		{
			// members
			register_class_members(ctx, cls);
		}

		DuktapeHeapObject *proto_ptr = duk_get_heapptr(ctx, -1);
		get_singleton()->native_class_prototypes[cls->name] = proto_ptr;

		duk_put_prop_literal(ctx, -2, PROTOTYPE_LITERAL);

	}
	duk_put_prop_godot_string_name(ctx, -2, cls->name);
}

DuktapeBindingHelper *DuktapeBindingHelper::get_singleton() {
	return dynamic_cast<DuktapeBindingHelper *>(ECMAScriptLanguage::get_binder());
}

ECMAScriptLanguage *DuktapeBindingHelper::get_language() {
	return ECMAScriptLanguage::get_singleton();
}

void DuktapeBindingHelper::initialize() {

	this->ctx = duk_create_heap(alloc_function, realloc_function, free_function, this, fatal_function);
	ERR_FAIL_NULL(ctx);

	// strong reference object pool
	duk_push_heap_stash(ctx);

	duk_push_object(ctx);
	this->strongref_pool_ptr = duk_get_heapptr(ctx, -1);
	duk_put_prop_literal(ctx, -2, "object_pool");
	{
		// pr-edefined functions for godot classes
		duk_push_c_function(ctx, duk_godot_object_finalizer, 1);
		this->duk_ptr_godot_object_finalizer = duk_get_heapptr(ctx, -1);
		duk_put_prop_literal(ctx, -2, "duk_godot_object_finalizer");

		duk_push_c_function(ctx, godot_object_virtual_method, 0);
		this->duk_ptr_godot_object_virtual_method = duk_get_heapptr(ctx, -1);
		duk_put_prop_literal(ctx, -2, "godot_object_virtual_method");

		duk_push_c_function(ctx, godot_object_free, 0);
		this->duk_ptr_godot_object_free = duk_get_heapptr(ctx, -1);
		duk_put_prop_literal(ctx, -2, "godot_object_free");

		duk_push_c_function(ctx, godot_object_to_string, 0);
		this->duk_ptr_godot_object_to_string = duk_get_heapptr(ctx, -1);
		duk_put_prop_literal(ctx, -2, "godot_object_to_string");
	}
	duk_pop(ctx);

	// global scope
	duk_push_c_function(ctx, godot_print_function, DUK_VARARGS);
	duk_put_global_literal(ctx, "print");

	// godot namespace
	duk_push_object(ctx);
	{
		// TODO: register builtin classes
		// register classes
		const StringName *key = ClassDB::classes.next(NULL);
		while (key) {
			const ClassDB::ClassInfo *cls = ClassDB::classes.getptr(*key);
			rigister_class(ctx, cls);
			key = ClassDB::classes.next(key);
		}
		// setup proto chain for prototypes
		key = ClassDB::classes.next(NULL);
		while (key) {
			const ClassDB::ClassInfo *cls = ClassDB::classes.getptr(*key);
			if (cls->inherits_ptr) {
				duk_require_stack(ctx, 2);
				DuktapeHeapObject *prototype_ptr = native_class_prototypes.get(cls->name);
				duk_push_heapptr(ctx, prototype_ptr);
				DuktapeHeapObject *base_prototype_ptr = native_class_prototypes.get(cls->inherits_ptr->name);
				duk_push_heapptr(ctx, base_prototype_ptr);
				duk_put_prop_literal(ctx, -2, PROTO_LITERAL);
				duk_pop(ctx);
			}
			key = ClassDB::classes.next(key);
		}

		// Singletons
		List<Engine::Singleton> singletons;
		Engine::get_singleton()->get_singletons(&singletons);
		for (List<Engine::Singleton>::Element* E = singletons.front(); E; E = E->next()) {
			const Engine::Singleton& s = E->get();

			ERR_CONTINUE(s.ptr == NULL);

			DuktapeHeapObject * prototype_ptr = native_class_prototypes.get(s.ptr->get_class_name());
			ERR_CONTINUE(prototype_ptr == NULL);

			duk_push_object(ctx);
			duk_push_heapptr(ctx, prototype_ptr);
			duk_put_prop_literal(ctx, -2, PROTO_LITERAL);
			duk_push_pointer(ctx, s.ptr);
			duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("ptr"));
			String name = s.name;
			duk_put_prop_string(ctx, -2, name.utf8().ptr());
		}
		// global constants
		HashMap<StringName, HashMap<StringName, int> > global_constants;
		for (int i = 0; i < GlobalConstants::get_global_constant_count(); ++i) {

			StringName enum_name = GlobalConstants::get_global_constant_enum(i);
			const char * const_name = GlobalConstants::get_global_constant_name(i);
			const int value = GlobalConstants::get_global_constant_value(i);

			duk_push_number(ctx, value);
			duk_put_prop_string(ctx, -2, const_name);

			if (HashMap<StringName, int> * consts = global_constants.getptr(enum_name)) {
				consts->set(const_name, value);
			} else {
				HashMap<StringName, int> enum_;
				enum_.set(const_name, value);
				global_constants.set(enum_name, enum_);
			}
		}

		// global enums
		for (const StringName * enum_name = global_constants.next(NULL); enum_name; enum_name = global_constants.next(enum_name)) {
			duk_push_object(ctx);
			const HashMap<StringName, int>& enum_ = global_constants.get(*enum_name);
			for (const StringName * const_name = enum_.next(NULL); const_name; const_name = enum_.next(const_name)) {
				String name = * const_name;
				const int value = enum_.get(*const_name);
				duk_push_number(ctx, value);
				duk_put_prop_string(ctx, -2, name.utf8().ptr());
			}
			String enum_name_str = *enum_name;
			duk_put_prop_string(ctx, -2, enum_name_str.utf8().ptr());
		}

		// godot.register_class
		duk_push_c_function(ctx, register_ecma_class, 3);
		this->duk_ptr_register_ecma_class = duk_get_heapptr(ctx, -1);
		duk_put_prop_literal(ctx, -2, "register_class");


#if 0
		// godot.GDCLASS
		duk_push_c_function(ctx, decorator_register_ecma_class, 3);
		duk_put_prop_literal(ctx, -2, "GDCLASS");
#endif
	}

	duk_put_global_literal(ctx, "godot");
}

void DuktapeBindingHelper::uninitialize() {
	duk_destroy_heap(ctx);
	this->ctx = NULL;
}

void DuktapeBindingHelper::register_class_members(duk_context *ctx, const ClassDB::ClassInfo *cls) {

	if (cls->name == "Object") {
		// Object.prototype.fin
		duk_push_heapptr(ctx, duk_ptr_godot_object_finalizer);
		duk_set_finalizer(ctx, -2);
		// Object.prototype.free
		duk_push_heapptr(ctx, duk_ptr_godot_object_free);
		duk_put_prop_literal(ctx, -2, "free");
		// Object.prototype.toString
		duk_push_heapptr(ctx, duk_ptr_godot_object_to_string);
		duk_put_prop_literal(ctx, -2, "toString");
	}

	const duk_idx_t prototype_idx = duk_get_top(ctx) - 1;

	{
		// methods
		const StringName *key = cls->method_map.next(NULL);
		while (key) {

			duk_require_stack(ctx, 2);

			MethodBind *mb = cls->method_map.get(*key);
			duk_push_godot_method(ctx, mb);
			duk_put_prop_godot_string_name(ctx, -2, mb->get_name());

			key = cls->method_map.next(key);
		}
	}

	{
		// properties
		const StringName *key = cls->property_setget.next(NULL);
		while (key) {

			duk_require_stack(ctx, 3);

			const ClassDB::PropertySetGet &prop = cls->property_setget[*key];
			duk_uidx_t masks = DUK_DEFPROP_FORCE;
			duk_push_godot_string_name(ctx, *key);
			if (prop._getptr) {
				duk_push_godot_method(ctx, prop._getptr);
				masks |= DUK_DEFPROP_HAVE_GETTER;
			}
			if (prop._setptr) {
				duk_push_godot_method(ctx, prop._setptr);
				masks |= DUK_DEFPROP_HAVE_SETTER;
			}

			if (masks) {
				duk_def_prop(ctx, prototype_idx, masks);
			} else {
				duk_pop(ctx);
			}
			key = cls->property_setget.next(key);
		}
	}
}

void DuktapeBindingHelper::duk_push_godot_method(duk_context *ctx, const MethodBind *mb) {

	DuktapeHeapObject **val_ele = method_bindings.getptr(mb);
	DuktapeHeapObject *heap_ptr = val_ele ? (*val_ele) : NULL;

	if (heap_ptr) {
		duk_push_heapptr(ctx, heap_ptr);
	} else {
		if (mb->get_hint_flags() & MethodFlags::METHOD_FLAG_VIRTUAL) {
			duk_push_heapptr(ctx, duk_ptr_godot_object_virtual_method);
			heap_ptr = duk_ptr_godot_object_virtual_method;
		} else {
			duk_size_t argc = DUK_VARARGS; // (mb->is_vararg() || mb->get_default_argument_count()) ? DUK_VARARGS : mb->get_argument_count();
			duk_push_c_function(ctx, duk_godot_object_method, argc);
			duk_push_pointer(ctx, (void *)mb);
			duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("mb"));
			heap_ptr = duk_get_heapptr(ctx, -1);
		}
		method_bindings[mb] = heap_ptr;
	}
}

duk_ret_t DuktapeBindingHelper::register_ecma_class(duk_context *ctx) {
	// godot.register_class(cls, name, icon)
	const duk_idx_t CLASS_FUNC_IDX = 0;

	ERR_FAIL_COND_V(!duk_is_function(ctx, CLASS_FUNC_IDX), DUK_ERR_TYPE_ERROR);

	duk_get_prop_literal(ctx, CLASS_FUNC_IDX, PROTOTYPE_LITERAL);
	duk_get_prop_literal(ctx, -1, DUK_HIDDEN_SYMBOL("cls"));
	ClassDB::ClassInfo *cls = static_cast<ClassDB::ClassInfo *>(duk_get_pointer(ctx, -1));
	duk_pop_2(ctx);
	ERR_FAIL_NULL_V(cls, DUK_ERR_TYPE_ERROR);

	const char *class_name = duk_get_string_default(ctx, 1, NULL);
	const char *class_icon = duk_get_string_default(ctx, 2, NULL);
	duk_push_current_function(ctx);
	if (duk_is_null_or_undefined(ctx, 1) && !class_name) {
		duk_get_prop_literal(ctx, -1, DUK_HIDDEN_SYMBOL("cls_name"));
		class_name = duk_get_string_default(ctx, -1, NULL);
		duk_pop(ctx);
		if (!class_name) {
			duk_get_prop_literal(ctx, -1, "name");
			class_name = duk_get_string_default(ctx, -1, NULL);
			duk_pop(ctx);
			ERR_FAIL_COND_V(class_name == NULL || !strlen(class_name), DUK_ERR_EVAL_ERROR);
		}
	}
	if (duk_is_null_or_undefined(ctx, 2) && !class_icon) {
		duk_get_prop_literal(ctx, -1, DUK_HIDDEN_SYMBOL("cls_icon"));
		class_icon = duk_get_string_default(ctx, -1, "");
		duk_pop(ctx);
	}
	duk_pop(ctx);

	ECMAClassInfo ecma_class;
	ecma_class.class_name = class_name;
	ecma_class.icon_path = class_icon;
	ecma_class.native_class = cls;
	ecma_class.ecma_constructor = { duk_get_heapptr(ctx, CLASS_FUNC_IDX) };

	// ecmascript methods
	duk_get_prop_literal(ctx, CLASS_FUNC_IDX, PROTOTYPE_LITERAL);
	// MyClass.prototype.class_name = 'MyClass';
	duk_push_string(ctx, class_name);
	duk_put_prop_literal(ctx, -2, ECMA_CLASS_NAME_LITERAL);

	// for (var key in MyClass.prototype)
	duk_enum(ctx, -1, DUK_HINT_NONE);
	while (duk_next(ctx, -1, true)) {
		if (duk_is_ecmascript_function(ctx, -1)) {
			StringName name = duk_get_godot_string(ctx, -2);
			ECMAMethodInfo method = { duk_get_heapptr(ctx, -1) };
			ecma_class.methods.set(name, method);
		}
		duk_pop_2(ctx);
	}
	duk_pop_2(ctx);

	duk_dup(ctx, CLASS_FUNC_IDX);
	// MyClass.class_name = 'MyClass';
	duk_push_string(ctx, class_name);
	duk_put_prop_literal(ctx, -2, ECMA_CLASS_NAME_LITERAL);

	get_singleton()->ecma_classes.set(class_name, ecma_class);

	Ref<ECMAScript> script;
	script.instance();
	script->class_name = class_name;
	get_language()->script_classes.set(class_name, script);

	return DUK_HAS_RET_VAL;
}

#if 0
duk_ret_t DuktapeBindingHelper::decorator_register_ecma_class(duk_context *ctx) {
	// godot.GDCLASS(name, icon)

	const char *class_name = duk_get_string_default(ctx, 0, NULL);
	const char *icon_path = duk_get_string_default(ctx, 1, NULL);

	duk_push_heapptr(ctx, get_singleton()->duk_ptr_register_ecma_class);
	duk_push_string(ctx, class_name);
	duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("cls_name"));
	duk_push_string(ctx, icon_path);
	duk_put_prop_literal(ctx, -2, DUK_HIDDEN_SYMBOL("cls_icon"));

	return DUK_HAS_RET_VAL;
}
#endif

ECMAScriptGCHandler DuktapeBindingHelper::create_ecma_instance_for_godot_object(const StringName &ecma_class_name, Object *p_object) {

	ECMAScriptGCHandler ret = { NULL };

	ECMAClassInfo *ecma_class = ecma_classes.getptr(ecma_class_name);
	ERR_FAIL_NULL_V(p_object, ret);
	ERR_FAIL_NULL_V(ecma_class, ret);

	duk_push_heapptr(ctx, ecma_class->ecma_constructor.ecma_object);
	ecma_instance_target = p_object;
	duk_new(ctx, 0);
	ecma_instance_target = NULL;
	ret.ecma_object = duk_get_heapptr(ctx, -1);
	set_strong_ref(p_object->get_instance_id(), ret.ecma_object);

	return ret;
}

Variant DuktapeBindingHelper::call_method(const ECMAScriptGCHandler &p_object, const ECMAMethodInfo &p_method, const Variant **p_args, int p_argcount, Variant::CallError &r_error) {
	if (p_object.is_null()) {
		r_error.error = Variant::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return NULL;
	}

	if (p_method.is_null()) {
		r_error.error = Variant::CallError::CALL_ERROR_INVALID_METHOD;
		return NULL;
	}
	duk_push_heapptr(ctx, p_method.ecma_object);
	duk_push_heapptr(ctx, p_object.ecma_object);

	for (int i = 0; i < p_argcount; ++i) {
		duk_push_godot_variant(ctx, *(p_args[i]));
	}
	duk_call_method(ctx, p_argcount);
	Variant ret = duk_get_godot_variant(ctx, -1);
	duk_pop(ctx);
	return ret;
}