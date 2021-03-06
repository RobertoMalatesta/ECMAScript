#ifndef ECMASCRIPT_BINDING_HELPER_H
#define ECMASCRIPT_BINDING_HELPER_H

#include "core/object.h"
#include "core/reference.h"

#define PROTOTYPE_LITERAL "prototype"
#define PROTO_LITERAL "__proto__"
#define ECMA_CLASS_NAME_LITERAL "class_name"

struct ECMAScriptGCHandler {
	void *ecma_object;
	bool is_null() const { return ecma_object == NULL; }
};

struct ECMAScriptBindingData : public ECMAScriptGCHandler {
	Object *godot_object;
	ObjectID instance_id;
};

typedef ECMAScriptGCHandler ECMAMethodInfo;

struct ECMAProperyInfo {
	Variant::Type type;
	Variant default_value;
};

struct ECMAClassInfo {
	ECMAScriptGCHandler ecma_constructor;
	StringName class_name;
	String icon_path;
	bool tool;
	ClassDB::ClassInfo *native_class;
	HashMap<StringName, ECMAMethodInfo> methods;
	HashMap<StringName, MethodInfo> signals;
	HashMap<StringName, ECMAProperyInfo> properties;
};

class ECMAScriptBindingHelper {
	friend class ECMAScript;

protected:
	HashMap<StringName, ECMAClassInfo> ecma_classes;

public:
	virtual void clear_classes() { ecma_classes.clear(); }

	virtual void initialize() = 0;
	virtual void uninitialize() = 0;

	virtual void *alloc_object_binding_data(Object *p_object) = 0;
	virtual void free_object_binding_data(void *p_gc_handle) = 0;

	virtual void godot_refcount_incremented(Reference *p_object) = 0;
	virtual bool godot_refcount_decremented(Reference *p_object) = 0;

	virtual Error eval_string(const String &p_source) = 0;
	virtual Error safe_eval_text(const String &p_source, String &r_error) = 0;

	virtual ECMAScriptGCHandler create_ecma_instance_for_godot_object(const StringName &ecma_class_name, Object *p_object) = 0;
	virtual Variant call_method(const ECMAScriptGCHandler &p_object, const ECMAMethodInfo &p_method, const Variant **p_args, int p_argcount, Variant::CallError &r_error) = 0;
	virtual bool get_instance_property(const ECMAScriptGCHandler &p_object, const StringName &p_name, Variant &r_ret) = 0;
	virtual bool set_instance_property(const ECMAScriptGCHandler &p_object, const StringName &p_name, const Variant &p_value) = 0;
};

#endif
