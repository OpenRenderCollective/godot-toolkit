extends VBoxContainer

signal command_requested(request: Dictionary)
signal parent_pick_requested(path: String)

var request: Callable
var project_directory := ""
var selected_material := ""
var schema_key := ""
var groups: Dictionary = {}
var controls: Dictionary = {}
var values: Dictionary = {}
var pending_spins: Dictionary = {}
var updating := false
var library_image: TextureRect
var parent_button: Button
var detach_button: Button
var revert_mesh_button: Button

func _label(text: String, parent: Node) -> Label:
	var label := Label.new(); label.text = text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	parent.add_child(label)
	return label

func _clear() -> void:
	updating = true
	controls.clear(); values.clear(); pending_spins.clear()
	library_image = null
	parent_button = null
	detach_button = null
	revert_mesh_button = null
	for child in get_children():
		remove_child(child); child.queue_free()
	updating = false

func _group(name: String, default_open := true) -> VBoxContainer:
	var header := Button.new()
	header.text = name; header.alignment = HORIZONTAL_ALIGNMENT_LEFT
	header.toggle_mode = true; header.button_pressed = groups.get(name, default_open)
	add_child(header)
	var body := VBoxContainer.new(); body.visible = header.button_pressed; add_child(body)
	header.toggled.connect(func(open: bool): groups[name] = open; body.visible = open)
	return body

func show_selection(details: Dictionary) -> void:
	if details.is_empty():
		if schema_key != "empty":
			_finish_editing(); _clear(); schema_key = "empty"; _label("Select an object to edit.", self)
		return
	var authoring: Dictionary = details.authoring
	var slots: Array = authoring.get("materials", [])
	if not slots.any(func(slot: Dictionary): return slot.path == selected_material):
		selected_material = slots[0].path if not slots.is_empty() else ""
	var material_details: Dictionary = request.call({"op": "inspect", "path": selected_material}) if not selected_material.is_empty() else {}
	var fields: Array = authoring.fields.duplicate(true)
	for field in fields:
		field["editable"] = authoring.get("editable", false)
	if not material_details.is_empty():
		for field in material_details.authoring.fields:
			field["editable"] = material_details.authoring.get("editable", false)
			fields.append(field)
	var key := JSON.stringify([details.path, details.get("attachment"), details.get("mesh_replacement"), authoring.kind, authoring.get("light_type"), authoring.get("references", []), selected_material,
		fields.map(func(field: Dictionary): return [field.path, field.property, field.type])])
	if schema_key != key:
		_finish_editing(); _clear(); schema_key = key
		_label(String(details.path).get_file(), self).add_theme_font_size_override("font_size", 16)
		var identity := _label(authoring.get("type", "Object"), self)
		identity.tooltip_text = details.path
		for reference in authoring.get("references", []):
			_label("Asset: " + str(reference), self).add_theme_font_size_override("font_size", 12)
		if details.get("anchor") != null:
			_label("Edits apply to this captured asset's instances.", self).add_theme_font_size_override("font_size", 12)
		if details.has("mesh_replacement"):
			revert_mesh_button = Button.new(); revert_mesh_button.text = "Revert mesh replacement"
			revert_mesh_button.tooltip_text = "Restore the captured mesh across its instances. Keeps added lights and meshes. Can be undone."
			revert_mesh_button.pressed.connect(func():
				_finish_editing()
				command_requested.emit({"op": "revert_mesh", "path": details.path, "label": "Revert mesh replacement"}))
			add_child(revert_mesh_button)
		if details.has("attachment"):
			_label("Parent", self)
			var parent: String = details.attachment.parent
			_label("World (unattached)" if parent.is_empty() else parent, self).tooltip_text = parent
			var row := HBoxContainer.new(); add_child(row)
			parent_button = Button.new(); parent_button.text = "Pick parent in viewport"
			parent_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			parent_button.pressed.connect(func(): _finish_editing(); parent_pick_requested.emit(details.attachment.path))
			row.add_child(parent_button)
			detach_button = Button.new(); detach_button.text = "Detach"; detach_button.disabled = parent.is_empty()
			detach_button.pressed.connect(func():
				_finish_editing()
				command_requested.emit({"op": "set_parent", "path": details.attachment.path, "parent": ""}))
			row.add_child(detach_button)
			_label("Pick a mesh in the viewport. Captured meshes share attachments across matching instances.", self).add_theme_font_size_override("font_size", 12)
		if details.has("translation") and authoring.get("editable", false):
			var body := _group("Transform", authoring.kind != "light")
			for property in ["translation", "rotation", "scale"]:
				_vector(details.path, property, details[property], body)
		elif details.has("translation"):
			_label("Select a mesh or light inside this instance to author changes.", self)
		if authoring.kind == "light":
			var row := HBoxContainer.new(); add_child(row)
			_label("Light type", row).autowrap_mode = TextServer.AUTOWRAP_OFF
			var types := ["SphereLight", "SpotLight", "RectLight", "DiskLight", "CylinderLight", "DistantLight", "DomeLight"]
			var choose := OptionButton.new(); choose.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			for type in types:
				choose.add_item(type.trim_suffix("Light"))
			choose.select(types.find(authoring.light_type)); row.add_child(choose)
			choose.item_selected.connect(func(index: int): command_requested.emit({"op": "set_light_type", "path": details.path, "type": types[index]}))
		if not slots.is_empty():
			_label("Bound material", self)
			var choose := OptionButton.new(); choose.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			for slot in slots:
				choose.add_item(slot.slot + " · " + slot.name)
				if slot.path == selected_material:
					choose.select(choose.item_count - 1)
			choose.item_selected.connect(func(index: int): selected_material = slots[index].path; show_selection(details))
			add_child(choose)
		if authoring.kind == "material" or not material_details.is_empty():
			_label("Material changes affect every mesh using this material.", self).add_theme_font_size_override("font_size", 12)
			var material_authoring: Dictionary = authoring if material_details.is_empty() else material_details.authoring
			var material_path: String = details.path if material_details.is_empty() else selected_material
			var row := HBoxContainer.new(); add_child(row)
			_label("Surface model", row).autowrap_mode = TextServer.AUTOWRAP_OFF
			var models := ["opaque", "translucent"]
			var model := OptionButton.new(); model.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			model.add_item("Opaque"); model.add_item("Translucent (glass, water)")
			model.select(models.find(material_authoring.get("surface_model", "opaque")))
			model.disabled = not material_authoring.get("editable", false)
			model.tooltip_text = "Translucent materials refract and tint the light passing through them. Converting keeps the material's texture."
			model.item_selected.connect(func(index: int):
				_finish_editing()
				command_requested.emit({"op": "set_material_model", "path": material_path, "model": models[index], "label": "Change surface model"}))
			row.add_child(model)
		var bodies := {}
		for field in fields:
			var group: String = field.group
			if not bodies.has(group):
				bodies[group] = _group(group, group != "Object")
			_field(field, bodies[group])
	updating = true
	for property in ["translation", "rotation", "scale"]:
		var id: String = details.path + "|" + property
		if controls.has(id):
			values[id] = details[property].duplicate()
			for i in range(3):
				var spin: SpinBox = controls[id].fields[i]
				if not spin.get_line_edit().has_focus():
					spin.set_value_no_signal(details[property][i])
	var field_values := {}
	for field in fields:
		field_values[field.path + "|" + field.property] = field.value
	for field in fields:
		_update_field(field, field_values)
	updating = false

func set_parent_picking(active: bool) -> void:
	if is_instance_valid(parent_button):
		parent_button.text = "Cancel parent picking" if active else "Pick parent in viewport"

func _vector(path: String, property: String, value: Array, parent: Node) -> void:
	_label(property.capitalize() + (" (degrees)" if property == "rotation" else ""), parent)
	var row := HBoxContainer.new(); parent.add_child(row)
	var id := path + "|" + property
	var spins: Array[SpinBox] = []
	for i in range(3):
		var spin := SpinBox.new(); spin.allow_greater = true; spin.allow_lesser = true
		spin.step = 0.01; spin.value = value[i]; spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		spin.custom_minimum_size.x = 60; spin.tooltip_text = ["X", "Y", "Z"][i]
		row.add_child(spin); spins.append(spin)
		_wire_spin(spin, id)
	controls[id] = {"fields": spins, "request": {"op": "transform", "path": path}, "property": property}
	values[id] = value.duplicate()

func _wire_spin(spin: SpinBox, id: String) -> void:
	spin.value_changed.connect(func(_value: float):
		if not updating:
			pending_spins[id] = true)
	spin.get_line_edit().text_submitted.connect(func(_text: String): _commit_spin.call_deferred(id))
	spin.get_line_edit().focus_exited.connect(func():
		if not updating and controls.has(id):
			spin.apply(); _commit_spin(id))

func _finish_editing() -> void:
	# Finish the old selection's edit before its controls are removed.
	for id in controls:
		var entry: Dictionary = controls[id]
		var spins: Array = entry.fields if entry.has("fields") else [entry.widget] if entry.widget is SpinBox else []
		for spin: SpinBox in spins:
			if spin.get_line_edit().has_focus():
				spin.apply()
		if pending_spins.has(id):
			_commit_spin(id)
		if entry.has("widget") and entry.widget is LineEdit and entry.widget.has_focus():
			entry.widget.text_submitted.emit(entry.widget.text)

func _input(event: InputEvent) -> void:
	if event is InputEventMouseButton and not event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
		for id in pending_spins.keys():
			_commit_spin.call_deferred(id)

func _commit_spin(id: String) -> void:
	if updating or not controls.has(id):
		return
	pending_spins.erase(id)
	var entry: Dictionary = controls[id]
	var next = entry.fields.map(func(spin: SpinBox): return spin.value) if entry.has("fields") else entry.widget.value
	if values.get(id) == next:
		return
	values[id] = next
	var command: Dictionary = entry.request.duplicate(true)
	command[entry.property if entry.has("fields") else "value"] = next
	command_requested.emit(command)

func _field(field: Dictionary, parent: Node) -> void:
	var id: String = field.path + "|" + field.property
	var header := HBoxContainer.new(); parent.add_child(header)
	_label(field.label, header).size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var reset := Button.new(); reset.text = "↺"; reset.tooltip_text = "Reset override in the active authoring layer"
	header.add_child(reset)
	reset.pressed.connect(func(): command_requested.emit({"op": "reset_authoring_property", "path": field.path, "property": field.property}))
	var command := {"op": "set_authoring_property", "path": field.path, "property": field.property, "label": field.label}
	var commit := func(value: Variant):
		if not updating and values.get(id) != value:
			values[id] = value
			var change: Dictionary = command.duplicate(true); change["value"] = value
			command_requested.emit(change)
	var widget: Control
	match field.control:
		"bool":
			var toggle := CheckButton.new(); toggle.toggled.connect(commit); widget = toggle
		"choice":
			var choose := OptionButton.new()
			for label in field.labels:
				choose.add_item(label)
			choose.item_selected.connect(func(index: int): commit.call(field.choices[index])); widget = choose
		"color":
			var color := ColorPickerButton.new(); color.edit_alpha = false; color.custom_minimum_size.y = 22
			color.popup_closed.connect(func(): commit.call([color.color.r, color.color.g, color.color.b])); widget = color
		"texture", "file":
			var texture: bool = field.control == "texture"
			var row := HBoxContainer.new(); parent.add_child(row)
			var text := LineEdit.new(); text.placeholder_text = "No texture" if texture else "No file"; text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			text.text_submitted.connect(func(value: String): commit.call(value)); row.add_child(text)
			text.focus_exited.connect(func():
				if controls.has(id): commit.call(text.text))
			var browse := Button.new(); browse.text = "…"; browse.tooltip_text = "Choose an ingested DDS texture" if texture else "Choose a file"
			browse.pressed.connect(func(): _choose_texture(commit, PackedStringArray(field.get("filters", ["*.dds ; DDS textures"])), texture)); row.add_child(browse)
			var clear := Button.new(); clear.text = "×"; clear.tooltip_text = "Clear texture" if texture else "Clear file"
			clear.pressed.connect(func(): commit.call("")); row.add_child(clear)
			widget = text
		"vector":
			var row := HBoxContainer.new(); parent.add_child(row)
			var spins: Array[SpinBox] = []
			for i in range(3):
				var spin := SpinBox.new(); spin.min_value = field.min; spin.max_value = field.max
				spin.step = 0.01; spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
				spin.custom_minimum_size.x = 60; spin.tooltip_text = ["X", "Y", "Z"][i]
				row.add_child(spin); spins.append(spin)
				_wire_spin(spin, id)
			row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.tooltip_text = field.get("tooltip", "")
			controls[id] = {"widget": row, "fields": spins, "reset": reset, "request": command, "property": "value"}
			return
		_:
			var number := SpinBox.new(); number.min_value = field.min; number.max_value = field.max
			number.step = 1.0 if field.type in ["int", "uint"] else 0.01
			_wire_spin(number, id); widget = number
	if widget.get_parent() == null:
		parent.add_child(widget)
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if field.has("tooltip"): widget.tooltip_text = field.tooltip
	if field.property == "inputs:radius":
		widget.tooltip_text = "Physical emitter radius in scene units, not illumination range. Keep the emitter outside surrounding geometry."
	controls[id] = {"widget": widget, "reset": reset, "request": command}

func _update_field(field: Dictionary, field_values: Dictionary) -> void:
	var id: String = field.path + "|" + field.property
	if not controls.has(id):
		return
	values[id] = field.value
	var widget: Control = controls[id].widget
	controls[id].reset.disabled = not field.get("overridden", false) or not field.get("editable", true)
	var enabled: bool = field.get("editable", true) and (not field.has("enabled_by") or bool(field_values.get(field.path + "|" + field.enabled_by, false)))
	enabled = enabled and (not field.has("disabled_by") or not bool(field_values.get(field.path + "|" + field.disabled_by, false)))
	if controls[id].has("fields"):
		for i in range(3):
			var spin: SpinBox = controls[id].fields[i]
			spin.editable = enabled
			if not spin.get_line_edit().has_focus():
				spin.set_value_no_signal(field.value[i])
		return
	widget.set_block_signals(true)
	if widget is SpinBox:
		widget.editable = enabled
		if not widget.get_line_edit().has_focus():
			widget.set_value_no_signal(field.value)
	elif widget is CheckButton:
		widget.button_pressed = field.value; widget.disabled = not enabled
	elif widget is OptionButton:
		widget.select(field.choices.find(field.value)); widget.disabled = not enabled
	elif widget is ColorPickerButton:
		widget.color = Color(field.value[0], field.value[1], field.value[2]); widget.disabled = not enabled
	elif widget is LineEdit:
		widget.editable = enabled
		if not widget.has_focus():
			widget.text = field.value
		widget.tooltip_text = field.get("resolved", "")
		for child in widget.get_parent().get_children():
			if child is Button:
				child.disabled = not enabled
	widget.set_block_signals(false)

func _choose_texture(commit: Callable, filters := PackedStringArray(["*.dds ; DDS textures"]), texture := true) -> void:
	var dialog := FileDialog.new(); dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE; dialog.title = "Assign ingested texture" if texture else "Assign file"
	dialog.filters = filters
	if not project_directory.is_empty():
		dialog.current_dir = project_directory.path_join("assets") if texture else project_directory
	add_child(dialog)
	dialog.file_selected.connect(func(path: String): commit.call(path); dialog.queue_free())
	dialog.canceled.connect(dialog.queue_free); dialog.popup_centered_ratio(0.65)

func show_asset(id: String, details: Dictionary) -> void:
	_finish_editing(); _clear(); schema_key = "library:" + id
	_label(details.name, self).add_theme_font_size_override("font_size", 16)
	_label(String(details.get("kind", "Asset")).capitalize() + " · Library asset", self)
	library_image = TextureRect.new(); library_image.texture = details.get("texture")
	library_image.custom_minimum_size = Vector2(160, 160)
	library_image.expand_mode = TextureRect.EXPAND_IGNORE_SIZE; library_image.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	add_child(library_image)
	_label(details.get("folder_path", "Assets"), self)
	_label(id, self).add_theme_font_size_override("font_size", 12)
	_label("Select a scene object to author changes. Add or replace uses the last selected scene object.", self)
	if not details.get("available", true):
		_label("This asset's files are missing.", self)
	var metadata_path: String = String(details.asset).get_base_dir().path_join("ingestion.json")
	if FileAccess.file_exists(metadata_path):
		var metadata = JSON.parse_string(FileAccess.get_file_as_string(metadata_path))
		if metadata is Dictionary:
			_label("Source: " + str(metadata.get("source", "Unknown")), self).add_theme_font_size_override("font_size", 12)
			for warning in metadata.get("warnings", []):
				_label(str(warning), self)
