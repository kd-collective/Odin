gb_internal TB_FunctionPrototype *cg_procedure_type_as_prototype(cgModule *m, Type *type) {
	GB_ASSERT(type != nullptr);
	type = base_type(type);
	GB_ASSERT(type->kind == Type_Proc);
	TypeProc *pt = &type->Proc;

	auto params = array_make<TB_PrototypeParam>(heap_allocator(), 0, pt->param_count);
	if (pt->params) for (Entity *e : pt->params->Tuple.variables) {
		TB_PrototypeParam param = {};

		Type *t = core_type(e->type);
		i64 sz = type_size_of(t);
		switch (t->kind) {
		case Type_Basic:
			switch (t->Basic.kind) {
			case Basic_bool:
			case Basic_b8:
			case Basic_b16:
			case Basic_b32:
			case Basic_b64:
			case Basic_i8:
			case Basic_u8:
			case Basic_i16:
			case Basic_u16:
			case Basic_i32:
			case Basic_u32:
			case Basic_i64:
			case Basic_u64:
			case Basic_i128:
			case Basic_u128:
			case Basic_rune:
			case Basic_int:
			case Basic_uint:
			case Basic_uintptr:
				param.dt = TB_TYPE_INTN(cast(u16)(8*sz));
				break;

			case Basic_f16: param.dt = TB_TYPE_I16; break;
			case Basic_f32: param.dt = TB_TYPE_F32; break;
			case Basic_f64: param.dt = TB_TYPE_F64; break;

			case Basic_complex32:
			case Basic_complex64:
			case Basic_complex128:
			case Basic_quaternion64:
			case Basic_quaternion128:
			case Basic_quaternion256:
				param.dt = TB_TYPE_PTR;
				break;


			case Basic_rawptr:
				param.dt = TB_TYPE_PTR;
				break;
			case Basic_string:  // ^u8 + int
				param.dt = TB_TYPE_PTR;
				break;
			case Basic_cstring: // ^u8
				param.dt = TB_TYPE_PTR;
				break;
			case Basic_any:     // rawptr + ^Type_Info
				param.dt = TB_TYPE_PTR;
				break;

			case Basic_typeid:
				param.dt = TB_TYPE_INTN(cast(u16)(8*sz));
				break;

			// Endian Specific Types
			case Basic_i16le:
			case Basic_u16le:
			case Basic_i32le:
			case Basic_u32le:
			case Basic_i64le:
			case Basic_u64le:
			case Basic_i128le:
			case Basic_u128le:
			case Basic_i16be:
			case Basic_u16be:
			case Basic_i32be:
			case Basic_u32be:
			case Basic_i64be:
			case Basic_u64be:
			case Basic_i128be:
			case Basic_u128be:
				param.dt = TB_TYPE_INTN(cast(u16)(8*sz));
				break;

			case Basic_f16le: param.dt = TB_TYPE_I16; break;
			case Basic_f32le: param.dt = TB_TYPE_F32; break;
			case Basic_f64le: param.dt = TB_TYPE_F64; break;

			case Basic_f16be: param.dt = TB_TYPE_I16; break;
			case Basic_f32be: param.dt = TB_TYPE_F32; break;
			case Basic_f64be: param.dt = TB_TYPE_F64; break;
			}

		case Type_Pointer:
		case Type_MultiPointer:
		case Type_Proc:
			param.dt = TB_TYPE_PTR;
			break;

		default:
			switch (sz) {
			case 1: param.dt = TB_TYPE_I8;  break;
			case 2: param.dt = TB_TYPE_I16; break;
			case 4: param.dt = TB_TYPE_I32; break;
			case 8: param.dt = TB_TYPE_I64; break;
			default:
				param.dt = TB_TYPE_PTR;
				break;
			}
		}

		if (param.dt.width != 0) {
			if (is_blank_ident(e->token)) {
				param.name = alloc_cstring(temporary_allocator(), e->token.string);
			}
			array_add(&params, param);
		}
	}

	auto results = array_make<TB_PrototypeParam>(heap_allocator(), 0, pt->result_count);
	// if (pt->results) for (Entity *e : pt->params->Tuple.variables) {
	// 	// TODO(bill):
	// }


	return tb_prototype_create(m->mod, TB_CDECL, params.count, params.data, results.count, results.data, pt->c_vararg);
}

gb_internal cgProcedure *cg_procedure_create(cgModule *m, Entity *entity, bool ignore_body=false) {
	GB_ASSERT(entity != nullptr);
	GB_ASSERT(entity->kind == Entity_Procedure);
	if (!entity->Procedure.is_foreign) {
		if ((entity->flags & EntityFlag_ProcBodyChecked) == 0) {
			GB_PANIC("%.*s :: %s (was parapoly: %d %d)", LIT(entity->token.string), type_to_string(entity->type), is_type_polymorphic(entity->type, true), is_type_polymorphic(entity->type, false));
		}
	}

	String link_name = cg_get_entity_name(m, entity);

	cgProcedure *p = nullptr;
	{
		StringHashKey key = string_hash_string(link_name);
		cgValue *found = string_map_get(&m->members, key);
		if (found) {
			cg_add_entity(m, entity, *found);
			p = string_map_must_get(&m->procedures, key);
			if (!ignore_body && p->func != nullptr) {
				return nullptr;
			}
		}
	}

	if (p == nullptr) {
		p = gb_alloc_item(permanent_allocator(), cgProcedure);
	}

	p->module = m;
	p->entity = entity;
	p->name = link_name;

	DeclInfo *decl = entity->decl_info;

	ast_node(pl, ProcLit, decl->proc_lit);
	Type *pt = base_type(entity->type);
	GB_ASSERT(pt->kind == Type_Proc);

	p->type           = entity->type;
	p->type_expr      = decl->type_expr;
	p->body           = pl->body;
	p->inlining       = pl->inlining;
	p->is_foreign     = entity->Procedure.is_foreign;
	p->is_export      = entity->Procedure.is_export;
	p->is_entry_point = false;

	gbAllocator a = heap_allocator();
	p->children.allocator      = a;
	// p->defer_stmts.allocator   = a;
	// p->blocks.allocator        = a;
	// p->branch_blocks.allocator = a;
	p->context_stack.allocator = a;
	p->scope_stack.allocator   = a;
	// map_init(&p->tuple_fix_map, 0);

	TB_Linkage linkage = TB_LINKAGE_PRIVATE;
	if (p->is_export) {
		linkage = TB_LINKAGE_PUBLIC;
	} else if (p->is_foreign || ignore_body) {
		if (ignore_body) {
			linkage = TB_LINKAGE_PUBLIC;
		}
		p->symbol = cast(TB_Symbol *)tb_extern_create(m->mod, link_name.len, cast(char const *)link_name.text, TB_EXTERNAL_SO_LOCAL);
	}

	if (p->symbol == nullptr)  {
		p->func = tb_function_create(m->mod, link_name.len, cast(char const *)link_name.text, linkage, TB_COMDAT_NONE);
		tb_function_set_prototype(p->func, cg_procedure_type_as_prototype(m, p->type), tb_default_arena());
		p->symbol = cast(TB_Symbol *)p->func;
	}

	cgValue proc_value = cg_value(p->symbol, p->type);
	cg_add_entity(m, entity, proc_value);
	cg_add_member(m, p->name, proc_value);
	cg_add_procedure_value(m, p);


	return p;
}

gb_internal cgProcedure *cg_procedure_create_dummy(cgModule *m, String const &link_name, Type *type) {
	auto *prev_found = string_map_get(&m->members, link_name);
	GB_ASSERT_MSG(prev_found == nullptr, "failed to create dummy procedure for: %.*s", LIT(link_name));

	cgProcedure *p = gb_alloc_item(permanent_allocator(), cgProcedure);

	p->module = m;
	p->name = link_name;

	p->type           = type;
	p->type_expr      = nullptr;
	p->body           = nullptr;
	p->tags           = 0;
	p->inlining       = ProcInlining_none;
	p->is_foreign     = false;
	p->is_export      = false;
	p->is_entry_point = false;

	gbAllocator a = heap_allocator();
	p->children.allocator      = a;
	// p->defer_stmts.allocator   = a;
	// p->blocks.allocator        = a;
	// p->branch_blocks.allocator = a;
	p->scope_stack.allocator = a;
	p->context_stack.allocator = a;
	// map_init(&p->tuple_fix_map, 0);


	TB_Linkage linkage = TB_LINKAGE_PRIVATE;

	p->func = tb_function_create(m->mod, link_name.len, cast(char const *)link_name.text, linkage, TB_COMDAT_NONE);
	tb_function_set_prototype(p->func, cg_procedure_type_as_prototype(m, p->type), tb_default_arena());
	p->symbol = cast(TB_Symbol *)p->func;

	cgValue proc_value = cg_value(p->symbol, p->type);
	cg_add_member(m, p->name, proc_value);
	cg_add_procedure_value(m, p);

	return p;
}

gb_internal void cg_procedure_begin(cgProcedure *p) {
	if (p == nullptr || p->func == nullptr) {
		return;
	}
}

gb_internal void cg_procedure_end(cgProcedure *p) {
	if (p == nullptr || p->func == nullptr) {
		return;
	}
	tb_inst_ret(p->func, 0, nullptr);
	if (p->name == "main") {
		TB_Arena *arena = tb_default_arena();
		defer (arena->free(arena));
		TB_FuncOpt *opt = tb_funcopt_enter(p->func, arena);
		defer (tb_funcopt_exit(opt));
		tb_funcopt_print(opt);
	}
	tb_module_compile_function(p->module->mod, p->func, TB_ISEL_FAST);
}

gb_internal void cg_procedure_generate(cgProcedure *p) {
	if (p->body == nullptr) {
		return;
	}
	cg_procedure_begin(p);
	defer (cg_procedure_end(p));

	if (p->name != "bug.main" &&
	    p->name != "main") {
		return;
	}
	cg_build_stmt(p, p->body);
}



gb_internal cgValue cg_find_procedure_value_from_entity(cgModule *m, Entity *e) {
	GB_ASSERT(is_type_proc(e->type));
	e = strip_entity_wrapping(e);
	GB_ASSERT(e != nullptr);
	GB_ASSERT(e->kind == Entity_Procedure);

	cgValue *found = nullptr;
	rw_mutex_shared_lock(&m->values_mutex);
	found = map_get(&m->values, e);
	rw_mutex_shared_unlock(&m->values_mutex);
	if (found) {
		return *found;
	}

	GB_PANIC("Error in: %s, missing procedure %.*s\n", token_pos_to_string(e->token.pos), LIT(e->token.string));
	return {};
}



gb_internal cgValue cg_build_call_expr_internal(cgProcedure *p, Ast *expr);
gb_internal cgValue cg_build_call_expr(cgProcedure *p, Ast *expr) {
	expr = unparen_expr(expr);
	ast_node(ce, CallExpr, expr);

	cgValue res = cg_build_call_expr_internal(p, expr);

	if (ce->optional_ok_one) { // TODO(bill): Minor hack for #optional_ok procedures
		GB_PANIC("Handle optional_ok_one");
		// GB_ASSERT(is_type_tuple(res.type));
		// GB_ASSERT(res.type->Tuple.variables.count == 2);
		// return cg_emit_struct_ev(p, res, 0);
	}
	return res;
}

gb_internal cgValue cg_emit_call(cgProcedure * p, cgValue value, Slice<cgValue> args) {
	if (value.kind == cgValue_Symbol) {
		value = cg_value(tb_inst_get_symbol_address(p->func, value.symbol), value.type);
	}
	GB_ASSERT(value.kind == cgValue_Value);

	TB_FunctionPrototype *proto = cg_procedure_type_as_prototype(p->module, value.type);
	TB_Node *target = value.node;
	auto params = slice_make<TB_Node *>(temporary_allocator(), 0);

	GB_ASSERT(target != nullptr);
	TB_MultiOutput multi_output = tb_inst_call(p->func, proto, target, 0, nullptr);
	gb_unused(multi_output);
	return {};
}

gb_internal cgValue cg_build_call_expr_internal(cgProcedure *p, Ast *expr) {
	ast_node(ce, CallExpr, expr);

	TypeAndValue tv = type_and_value_of_expr(expr);

	TypeAndValue proc_tv = type_and_value_of_expr(ce->proc);
	AddressingMode proc_mode = proc_tv.mode;
	if (proc_mode == Addressing_Type) {
		GB_ASSERT(ce->args.count == 1);
		cgValue x = cg_build_expr(p, ce->args[0]);
		return cg_emit_conv(p, x, tv.type);
	}

	Ast *proc_expr = unparen_expr(ce->proc);
	if (proc_mode == Addressing_Builtin) {
		Entity *e = entity_of_node(proc_expr);
		BuiltinProcId id = BuiltinProc_Invalid;
		if (e != nullptr) {
			id = cast(BuiltinProcId)e->Builtin.id;
		} else {
			id = BuiltinProc_DIRECTIVE;
		}
		if (id == BuiltinProc___entry_point) {
			if (p->module->info->entry_point) {
				cgValue entry_point = cg_find_procedure_value_from_entity(p->module, p->module->info->entry_point);
				GB_ASSERT(entry_point.node != nullptr);
				cg_emit_call(p, entry_point, {});
			}
			return {};
		}
		GB_PANIC("TODO(bill): builtin procs %d %.*s", id, LIT(builtin_procs[id].name));
	}

	// NOTE(bill): Regular call
	cgValue value = {};

	Entity *proc_entity = entity_of_node(proc_expr);
	if (proc_entity != nullptr) {
		if (proc_entity->flags & EntityFlag_Disabled) {
			GB_ASSERT(tv.type == nullptr);
			return {};
		}
	}

	if (proc_expr->tav.mode == Addressing_Constant) {
		ExactValue v = proc_expr->tav.value;
		switch (v.kind) {
		case ExactValue_Integer:
			{
				u64 u = big_int_to_u64(&v.value_integer);
				cgValue x = cg_value(tb_inst_uint(p->func, TB_TYPE_PTR, u), t_rawptr);
				value = cg_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		case ExactValue_Pointer:
			{
				u64 u = cast(u64)v.value_pointer;
				cgValue x = cg_value(tb_inst_uint(p->func, TB_TYPE_PTR, u), t_rawptr);
				value = cg_emit_conv(p, x, proc_expr->tav.type);
				break;
			}
		}
	}

	if (value.node == nullptr) {
		value = cg_build_expr(p, proc_expr);
	}
	if (value.kind == cgValue_Addr) {
		value = cg_emit_load(p, value);
	}
	GB_ASSERT(value.kind == cgValue_Value);
	GB_ASSERT(is_type_proc(value.type));

	return cg_emit_call(p, value, {});
}