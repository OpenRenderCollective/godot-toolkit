extends Control

const LayoutView = preload("res://scripts/layout_view.gd")
const RendererClient = preload("res://scripts/renderer_client.gd")
const AssetBrowser = preload("res://scripts/asset_browser.gd")
const AuthoringInspector = preload("res://scripts/authoring_inspector.gd")
const ProjectSessions = preload("res://scripts/project_sessions.gd")
const WelcomeScreen = preload("res://scripts/welcome_screen.gd")
const StudioTheme = preload("res://scripts/studio_theme.gd")
const RenderSettings = preload("res://scripts/render_settings.gd")
const RenderSettingsDialog = preload("res://scripts/render_settings_dialog.gd")
const GameLink = preload("res://scripts/game_link.gd")
const GameLinkDialog = preload("res://scripts/game_link_dialog.gd")
const MODEL_EXTENSIONS := ["usd", "usda", "usdc", "gltf", "glb", "fbx", "obj"]
const IMAGE_EXTENSIONS := ["png", "jpg", "jpeg", "tga", "exr", "hdr", "dds"]
var game_link := GameLink.new()
var game_link_button: Button
var game_link_dialog: AcceptDialog
var sessions := ProjectSessions.new()
var render_settings := RenderSettings.new()
var render_settings_dialog: ConfirmationDialog
var renderer_alert: AcceptDialog
var welcome: Control
var editor_page: Control
var camera_mode: OptionButton
var renderer_select: OptionButton
var runtime_paths: Dictionary = {}
var render_toggle: CheckButton
var dpi_timer: Timer
var restoring_view := false
var save_pending := false
var close_pending := false
var preview_jobs := 0
var save_serials: Dictionary = {}
var document: Object
var state: Dictionary = {}
var selection := ""
var tree: Tree
var assets: Control
var inspector: VBoxContainer
var layer_picker: OptionButton
var capture_picker: OptionButton
var add_capture_button: Button
var status: Label
var project_label: Label
var output: RichTextLabel
var viewport: Control
var renderer: Node
var pending_capture := ""
var refreshing := false
var import_running := false
var last_scene: Dictionary = {}
var runtime_path := ""
var runtime_dialog: FileDialog
var root_directory := ""
var library_selection := ""
var import_folder := ""
var import_project := ""
var import_workfile := ""
var import_queue: Array[Dictionary] = []
var import_dialog: ConfirmationDialog
var import_button: Button
var import_total := 0

func _ready() -> void:
	root_directory = ProjectSettings.globalize_path("res://").get_base_dir().get_base_dir()
	if ClassDB.class_exists("RemixDocument"):
		document = ClassDB.instantiate("RemixDocument")
	theme = StudioTheme.create()
	_update_dpi()
	_build_ui()
	game_link.document = document
	game_link.warning.connect(_log)
	game_link.status_changed.connect(_refresh_game_link)
	add_child(game_link)
	sessions.warning.connect(_log)
	sessions.load_history()
	render_settings.warning.connect(_log)
	render_settings.load_settings()
	renderer_select.select(1 if render_settings.renderer == "openremix" else 0)
	dpi_timer = Timer.new(); dpi_timer.wait_time = 1.0
	dpi_timer.timeout.connect(_update_dpi); add_child(dpi_timer); dpi_timer.start()
	get_window().dpi_changed.connect(_update_dpi)
	get_window().files_dropped.connect(_files_dropped)
	renderer = RendererClient.new()
	add_child(renderer)
	renderer.status_changed.connect(_log)
	renderer.failed.connect(_renderer_failed)
	renderer.response_received.connect(_renderer_response)
	renderer.texture_ready.connect(func(texture: Texture2D):
		if texture == null:
			viewport.rendered_texture = null)
	renderer.frame_presented.connect(func(context: Dictionary):
		if context.get("revision", -1) == state.get("revision", -2):
			viewport.rendered_texture = renderer.frame_texture)
	_show_welcome()
	get_tree().auto_accept_quit = false
	if document == null:
		_log("Native USD extension is unavailable. Build the project with scripts/bootstrap.py first.")
	else:
		_log("Ready. Open a Toolkit workfile or create a project from a Remix capture.")
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--project="):
			_open_project(argument.trim_prefix("--project="))

func _update_dpi() -> void:
	var factor := DisplayServer.screen_get_scale()
	if not is_finite(factor) or factor <= 0:
		factor = 1
	if dpi_timer == null and factor != 1.0 and DisplayServer.get_name() != "headless" and get_window().mode == Window.MODE_WINDOWED:
		var available := DisplayServer.screen_get_usable_rect()
		get_window().size = Vector2i(Vector2(get_window().size) * factor).min(available.size)
		get_window().position = available.position + (available.size - get_window().size) / 2
	if get_window().content_scale_mode != Window.CONTENT_SCALE_MODE_DISABLED or not is_equal_approx(get_window().content_scale_factor, factor):
		StudioTheme.apply_scale(get_window(), factor)
		if viewport != null:
			viewport._sync_view()

func _button(text: String, action: Callable, parent: Node) -> Button:
	var button := Button.new()
	button.text = text
	button.pressed.connect(action)
	parent.add_child(button)
	return button

func _label(text: String, parent: Node, font_size := 13) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", font_size)
	parent.add_child(label)
	return label

func _build_ui() -> void:
	var margin := MarginContainer.new()
	editor_page = margin
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 8)
	add_child(margin)
	var column := VBoxContainer.new()
	margin.add_child(column)
	var header := HBoxContainer.new()
	column.add_child(header)
	_label("REMIX STUDIO", header, 16).modulate = Color("70d9c0")
	project_label = _label("No project", header)
	project_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	project_label.custom_minimum_size.x = 80
	project_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_button("Recent projects", _show_welcome, header)
	_button("New from capture", _new_capture, header)
	_button("Open", _choose_open, header)
	_button("Save", _save_from_ui, header)
	_button("Package mod", _choose_package, header)
	var toolbar := HFlowContainer.new()
	column.add_child(toolbar)
	game_link_button = _button("Link to game…", _show_game_link, toolbar)
	game_link_button.disabled = true
	_button("Undo", func(): _command({"op": "undo"}), toolbar)
	_button("Redo", func(): _command({"op": "redo"}), toolbar)
	import_button = _button("Ingest asset", _choose_ingest, toolbar)
	import_button.tooltip_text = "Choose assets, or drop model/image files into the window to ingest them."
	_button("Cancel import", _cancel_import, toolbar)
	_button("Add mesh", func(): _use_asset("add_mesh"), toolbar)
	_button("Replace mesh", func(): _use_asset("replace_mesh"), toolbar)
	_button("Replace material", func(): _use_asset("replace_material"), toolbar)
	var lights := MenuButton.new()
	lights.text = "Add light"
	toolbar.add_child(lights)
	var types := ["SphereLight", "SpotLight", "RectLight", "DiskLight", "CylinderLight", "DistantLight", "DomeLight"]
	for type in types:
		lights.get_popup().add_item(type.trim_suffix("Light"))
	lights.get_popup().id_pressed.connect(func(id: int): _spawn_light(types[id]))
	_button("Duplicate", func(): _command({"op": "duplicate", "path": selection}), toolbar)
	_button("Delete", func(): _command({"op": "delete", "path": selection}), toolbar)
	var authoring_area := VSplitContainer.new()
	authoring_area.size_flags_vertical = Control.SIZE_EXPAND_FILL
	authoring_area.split_offset = 110
	column.add_child(authoring_area)
	var workspace := HSplitContainer.new()
	workspace.size_flags_vertical = Control.SIZE_EXPAND_FILL
	authoring_area.add_child(workspace)
	var left := VBoxContainer.new()
	left.custom_minimum_size.x = 210
	workspace.add_child(left)
	var capture_tools := HBoxContainer.new()
	left.add_child(capture_tools)
	_label("CAPTURE", capture_tools, 12).size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_capture_button = _button("Add capture", _choose_add_capture, capture_tools)
	add_capture_button.disabled = true
	capture_picker = OptionButton.new()
	capture_picker.fit_to_longest_item = false
	capture_picker.clip_text = true
	capture_picker.disabled = true
	capture_picker.item_selected.connect(func(index: int):
		if not refreshing:
			_change_capture(capture_picker.get_item_metadata(index)))
	left.add_child(capture_picker)
	_label("SCENE", left)
	tree = Tree.new()
	tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tree.hide_root = true
	tree.item_selected.connect(_tree_selected)
	left.add_child(tree)
	_label("AUTHORING LAYER", left, 12)
	layer_picker = OptionButton.new()
	left.add_child(layer_picker)
	layer_picker.item_selected.connect(func(index: int):
		if not refreshing:
			_command({"op": "select_layer", "layer": layer_picker.get_item_metadata(index)}))
	var center_right := HSplitContainer.new()
	center_right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	workspace.add_child(center_right)
	var center_panel := VBoxContainer.new()
	center_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center_right.add_child(center_panel)
	var view_tools := HFlowContainer.new()
	center_panel.add_child(view_tools)
	_label("Viewport", view_tools).size_flags_horizontal = Control.SIZE_EXPAND_FILL
	render_toggle = CheckButton.new()
	var renderer_label := Label.new(); renderer_label.text = "Renderer"
	view_tools.add_child(renderer_label)
	renderer_select = OptionButton.new()
	renderer_select.add_item("Remix"); renderer_select.add_item("OpenRemix")
	renderer_select.select(1 if render_settings.renderer == "openremix" else 0)
	renderer_select.item_selected.connect(_select_renderer)
	view_tools.add_child(renderer_select)
	render_toggle.text = "Render"
	render_toggle.button_pressed = true
	render_toggle.toggled.connect(func(value: bool):
		viewport.show_render = value
		_sync_renderer())
	view_tools.add_child(render_toggle)
	_button("Render settings", _show_render_settings, view_tools)
	camera_mode = OptionButton.new()
	camera_mode.add_item("Drone", LayoutView.CameraMode.DRONE)
	camera_mode.add_item("Orbit", LayoutView.CameraMode.ORBIT)
	camera_mode.tooltip_text = "Drone: press Z over the viewport for WASD movement and mouse look. Press Z or Escape to release."
	camera_mode.item_selected.connect(func(index: int): viewport.camera_mode = camera_mode.get_item_id(index))
	view_tools.add_child(camera_mode)
	var transform_tool := OptionButton.new()
	transform_tool.add_item("Move", 0); transform_tool.add_item("Rotate", 1)
	transform_tool.tooltip_text = "Drag the colored axes or rings. Snap uses 1 unit / 15°. Escape cancels a drag."
	transform_tool.item_selected.connect(func(index: int): viewport.gizmo.set_mode(index))
	view_tools.add_child(transform_tool)
	_button("Frame all", func(): viewport.frame_selection(true), view_tools)
	_button("Frame selection", func(): viewport.frame_selection(), view_tools)
	var snap := CheckButton.new()
	snap.text = "Snap"
	snap.toggled.connect(func(value: bool): viewport.snap = value)
	view_tools.add_child(snap)
	viewport = LayoutView.new()
	viewport.size_flags_vertical = Control.SIZE_EXPAND_FILL
	viewport.custom_minimum_size = Vector2(400, 300)
	viewport.selected.connect(_select)
	viewport.parent_picked.connect(func(path: String, parent: String): _command.call_deferred({"op": "set_parent", "path": path, "parent": parent}))
	viewport.parent_pick_ended.connect(func(): inspector.set_parent_picking(false))
	viewport.camera_changed.connect(func(camera: Dictionary):
		if renderer != null and renderer.is_ready and not restoring_view:
			renderer.send({"op": "camera", "camera": camera}))
	viewport.translation_requested.connect(_translate)
	viewport.transform_committed.connect(func(path: String, matrix: Array): _command({"op": "transform", "path": path, "world_matrix": matrix}))
	viewport.transform_preview_requested.connect(func(path: String, delta: Array):
		if renderer != null and renderer.is_ready:
			renderer.send({"op": "preview_transform", "path": path, "delta": delta, "revision": state.revision}))
	center_panel.add_child(viewport)
	_label("G: move · R: rotate · S: scale · X/Y/Z: lock axis · Shift+T: aim light · Ctrl+T: place at cursor. Left click confirms; Esc cancels.", center_panel, 11).autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var right := VBoxContainer.new()
	right.custom_minimum_size.x = 260
	center_right.add_child(right)
	_label("PROPERTIES", right)
	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	right.add_child(scroll)
	inspector = AuthoringInspector.new()
	inspector.request = _request
	inspector.parent_pick_requested.connect(_begin_parent_pick)
	inspector.command_requested.connect(func(command: Dictionary): _command.call_deferred(command))
	inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(inspector)
	assets = AssetBrowser.new()
	authoring_area.add_child(assets)
	assets.message.connect(_log)
	assets.asset_selected.connect(func(id: String, details: Dictionary):
		library_selection = id
		inspector.show_asset(id, details))
	assets.asset_details_changed.connect(func(id: String, details: Dictionary):
		if library_selection == id:
			inspector.show_asset(id, details))
	assets.thumbnail_changed.connect(func(id: String, texture: Texture2D):
		if library_selection == id and inspector.library_image != null:
			inspector.library_image.texture = texture)
	assets.asset_activated.connect(func(kind: String): _use_asset("replace_material" if kind == "material" else "add_mesh"))
	var output_toggle := Button.new()
	output_toggle.text = "Show output"
	output_toggle.toggle_mode = true
	output_toggle.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	column.add_child(output_toggle)
	var output_panel := VBoxContainer.new()
	output_panel.visible = false
	column.add_child(output_panel)
	output_toggle.toggled.connect(func(expanded: bool):
		output_panel.visible = expanded
		output_toggle.text = "Hide output" if expanded else "Show output")
	output = RichTextLabel.new()
	output.custom_minimum_size.y = 56
	output.scroll_following = true
	output_panel.add_child(output)
	status = _label("", output_panel, 11)
	status.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	welcome = WelcomeScreen.new(); add_child(welcome)
	welcome.browse.connect(_choose_open)
	welcome.create_project.connect(_new_capture)
	welcome.return_to_editor.connect(_show_editor)
	welcome.open_project.connect(func(path: String): _guard_unsaved(func(): _open_project(path)))
	welcome.remove_recent.connect(func(path: String): sessions.forget(path); welcome.refresh(sessions, state.get("open", false)))

func _log(message: String) -> void:
	status.text = message
	output.append_text(message + "\n")
	if welcome != null:
		welcome.notice.text = message

func _request(data: Dictionary) -> Dictionary:
	if document == null:
		_log("Build the native USD extension before opening a project.")
		return {}
	var response = document.execute_data(JSON.stringify(data))
	if not response is Dictionary or not response.get("ok", false):
		_log(response.get("error", "Invalid native response") if response is Dictionary else "Invalid native response")
		return {}
	var result = response.get("result", {})
	if not result is Dictionary:
		_log("Invalid native response for " + String(data.get("op", "request")))
		return {}
	return result

func _command(data: Dictionary) -> void:
	viewport.cancel_parent_pick()
	if data.op == "open" and not FileAccess.file_exists(data.path):
		_log("Project file is missing: " + data.path)
		return
	if data.op in ["open", "create"]:
		_persist_camera()
		game_link.flush()
	var previous_revision: int = state.get("revision", -1)
	var result := _request(data)
	if result.is_empty():
		if data.op in ["add_capture", "select_capture"]:
			_refresh_captures()
		if data.op in ["set_authoring_property", "reset_authoring_property", "set_light_type", "transform", "set_parent"] and library_selection.is_empty():
			_select(selection)
		return
	if result.has("selection"):
		selection = result.selection
	if result.has("document"):
		if result.document.get("path", "") != state.get("path", ""):
			_cancel_import()
		var capture_changed: bool = result.document.get("active_capture", "") != state.get("active_capture", "")
		if capture_changed and data.op not in ["open", "create"]:
			_persist_camera()
		if data.op in ["open", "create"] or capture_changed:
			inspector._clear(); inspector.schema_key = ""
			library_selection = ""; selection = ""; last_scene = {}
			viewport.set_mouse_capture(false); viewport.rendered_texture = null
		state = result.document
		_refresh(state.get("revision", -1) != previous_revision, result.get("scene_update") == "lights")
		if capture_changed:
			_sync_renderer()
	if data.op in ["open", "create"]:
		if is_instance_valid(game_link_dialog): game_link_dialog.queue_free()
		game_link.open_project()
		sessions.opened(state.path)
		_show_editor()
	elif state.get("revision", -1) != previous_revision:
		game_link.changed()
	if data.op == "save":
		if game_link.state.get("linked", false): game_link.sync_now()
		_log("Project saved.")
		_save_view_and_preview()
	elif data.op == "package":
		_log("Mod packaged at " + data.path)

func _refresh(scene_changed := true, lights_only := false) -> void:
	refreshing = true
	_refresh_captures()
	project_label.text = String(state.get("path", "No project")).get_file() + (" •" if state.get("dirty", false) else "")
	tree.clear()
	var root := tree.create_item()
	var entries := {"/": root}
	for prim in state.get("prims", []):
		var parent: String = String(prim.path).get_base_dir()
		var item := tree.create_item(entries.get(parent, root))
		item.set_text(0, prim.name + ("  [" + prim.type + "]" if not String(prim.type).is_empty() else ""))
		item.set_metadata(0, prim.path)
		item.set_tooltip_text(0, prim.path)
		if not prim.visible:
			item.set_custom_color(0, Color("74818e"))
		entries[prim.path] = item
		if prim.path == selection:
			item.select(0)
	# Inactive USD prims can still resolve by path, but are absent from the tree.
	if not selection.is_empty() and not entries.has(selection):
		selection = ""; library_selection = ""
	layer_picker.clear()
	for layer in state.get("layers", []):
		layer_picker.add_item(layer.name + (" (capture)" if layer.capture else ""))
		var i := layer_picker.item_count - 1
		layer_picker.set_item_metadata(i, layer.id)
		layer_picker.set_item_disabled(i, not layer.editable)
		if layer.selected:
			layer_picker.select(i)
	refreshing = false
	_refresh_assets()
	if library_selection.is_empty():
		_select(selection)
	if scene_changed and state.get("open", false):
		var first := last_scene.is_empty()
		lights_only = lights_only and not first
		if lights_only:
			var changes := _request({"op": "lights"})
			if not changes.has_all(["lights", "revision"]):
				return
			# Queued scene messages may still hold the old dictionary on the writer
			# thread. Share immutable geometry, but never mutate that dictionary.
			last_scene = last_scene.duplicate()
			last_scene["lights"] = changes.lights
			last_scene["revision"] = changes.revision
			for warning in changes.get("warnings", []): _log(warning)
		else:
			var previous_sources: Array = last_scene.get("category_sources", [])
			var next_scene := _request({"op": "scene"})
			if not next_scene.has_all(["revision", "meshes", "materials", "lights"]):
				# Do not send an empty/stale snapshot to the renderer. Keep the
				# document open so edits can still be saved and loading retried.
				last_scene = {}
				viewport.set_scene({})
				renderer.stop_worker()
				return
			last_scene = next_scene
			last_scene["geometry_revision"] = last_scene.revision
			var category_sources: Array = last_scene.get("category_sources", [])
			if (first or category_sources != previous_sources) and not category_sources.is_empty():
				_log("Remix categories: " + ", ".join(PackedStringArray(category_sources)))
		restoring_view = first
		viewport.set_scene(last_scene, false, lights_only)
		if first:
			var camera = sessions.camera_for(state.path, state.get("active_capture", ""))
			if not viewport.restore_view_state(camera, false):
				viewport.reset_view()
			camera_mode.select(viewport.camera_mode)
		restoring_view = false
		if renderer.is_ready:
			if lights_only:
				renderer.send({"op": "lights", "lights": last_scene.lights, "revision": last_scene.revision, "geometry_revision": last_scene.geometry_revision})
			else:
				renderer.send({"op": "scene", "scene": last_scene})
				renderer.send({"op": "camera", "camera": viewport.camera()})
		if not lights_only:
			for warning in state.get("warnings", []) + last_scene.get("warnings", []): _log(warning)

func _tree_selected() -> void:
	if refreshing:
		return
	var item := tree.get_selected()
	if item:
		_select(item.get_metadata(0))

func _select(path: String) -> void:
	viewport.cancel_parent_pick()
	var details := _request({"op": "inspect", "path": path}) if not path.is_empty() else {}
	if details.is_empty(): path = ""
	selection = path
	library_selection = ""
	viewport.selected_path = path
	viewport.queue_redraw()
	inspector.project_directory = String(state.get("path", "")).get_base_dir()
	inspector.show_selection(details)
	viewport.gizmo.set_target(details)

func _begin_parent_pick(path: String) -> void:
	if not viewport.parent_pick_path.is_empty():
		viewport.cancel_parent_pick(); return
	# Pending inspector changes are dispatched on the next frame.
	await get_tree().process_frame
	var details := _request({"op": "inspect", "path": path})
	if not details.has("attachment"): return
	var candidates := _request({"op": "parent_candidates", "path": path})
	if not candidates.has("parents"): return
	viewport.begin_parent_pick(path, candidates.parents, details.authoring.edit_path)
	inspector.set_parent_picking(true)

func _spawn_light(type: String) -> void:
	var defaults := _request({"op": "light_defaults", "type": type})
	if defaults.is_empty(): return
	var pose: Transform3D = viewport.light_spawn_transform(type, defaults.properties)
	library_selection = ""
	_command({"op": "add_light", "type": type, "world_matrix": viewport.TransformGizmo.to_array(pose)})

func _place_selection_at_cursor() -> void:
	if selection.is_empty() or not state.get("open", false): return
	var position: Vector2 = viewport._view_rect().get_center() if viewport.mouse_captured else viewport.get_local_mouse_position()
	if not viewport._view_rect().has_point(position): return
	var details := _request({"op": "inspect", "path": selection})
	if not details.has("world_transform") or not details.get("authoring", {}).get("editable", false): return
	var hit: Dictionary = viewport.surface_at(position, selection)
	if hit.is_empty():
		_log("No surface under the cursor.")
		return
	viewport.gizmo.cancel()
	var pose: Transform3D = viewport.placement_transform(viewport.TransformGizmo.from_array(details.world_transform), hit, details)
	library_selection = ""
	_command({"op": "transform", "path": selection, "world_matrix": viewport.TransformGizmo.to_array(pose)})

func _translate(delta: Vector3) -> void:
	_command({"op": "transform", "path": selection, "world_delta": [delta.x, delta.y, delta.z]})

func _dialog(mode: FileDialog.FileMode, title: String, filters: PackedStringArray, callback: Callable, dialog_parent: Node = null) -> FileDialog:
	var dialog := FileDialog.new()
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = mode
	dialog.title = title
	dialog.filters = filters
	dialog.use_native_dialog = false
	(dialog_parent if dialog_parent != null else self).add_child(dialog)
	if mode == FileDialog.FILE_MODE_OPEN_DIR:
		dialog.dir_selected.connect(func(path: String): dialog.hide(); dialog.queue_free(); callback.call_deferred(path))
	elif mode == FileDialog.FILE_MODE_OPEN_FILES:
		dialog.files_selected.connect(func(paths: PackedStringArray): dialog.hide(); dialog.queue_free(); callback.call_deferred(paths))
	else:
		dialog.file_selected.connect(func(path: String): dialog.hide(); dialog.queue_free(); callback.call_deferred(path))
	dialog.canceled.connect(dialog.queue_free)
	dialog.popup_centered_ratio(0.65)
	return dialog

func _guard_unsaved(action: Callable, canceled := Callable()) -> void:
	inspector._finish_editing()
	await get_tree().process_frame
	if not state.get("dirty", false):
		action.call()
		return
	var dialog := ConfirmationDialog.new()
	dialog.dialog_text = "This project has unsaved edits. Discard them?"
	dialog.ok_button_text = "Discard edits"
	add_child(dialog)
	dialog.confirmed.connect(func(): dialog.queue_free(); action.call())
	dialog.canceled.connect(func():
		dialog.queue_free()
		if canceled.is_valid(): canceled.call())
	dialog.popup_centered()

func _choose_open() -> void:
	_guard_unsaved(func(): _dialog(FileDialog.FILE_MODE_OPEN_FILE, "Open Toolkit project", ["*.usd,*.usda,*.usdc ; USD projects"], _open_project))

func _open_project(path: String) -> void:
	_command({"op": "open", "path": path})

func _refresh_captures() -> void:
	capture_picker.clear()
	add_capture_button.disabled = not state.get("open", false)
	for capture in state.get("captures", []):
		capture_picker.add_item(capture.name + (" (missing)" if capture.missing else ""))
		var index := capture_picker.item_count - 1
		capture_picker.set_item_metadata(index, capture.id)
		capture_picker.get_popup().set_item_tooltip(index, capture.path)
		capture_picker.set_item_disabled(index, capture.missing)
		if capture.active:
			capture_picker.select(index)
			capture_picker.tooltip_text = capture.path
	capture_picker.disabled = capture_picker.item_count == 0

func _choose_add_capture() -> void:
	if not state.get("open", false):
		return
	inspector._finish_editing()
	await get_tree().process_frame
	_dialog(FileDialog.FILE_MODE_OPEN_FILE, "Add capture to project", ["*.usd,*.usda,*.usdc ; Remix captures"], func(path: String): _change_capture(path, true))

func _change_capture(path: String, add := false) -> void:
	inspector._finish_editing()
	await get_tree().process_frame
	_command({"op": "add_capture" if add else "select_capture", "path": path})

func _new_capture() -> void:
	_guard_unsaved(func(): _dialog(FileDialog.FILE_MODE_OPEN_FILE, "Select Remix capture", ["*.usd,*.usda,*.usdc ; Remix captures"], func(path: String):
		pending_capture = path
		_dialog(FileDialog.FILE_MODE_SAVE_FILE, "Create project in a new directory", ["*.usda ; USD project"], func(project: String):
			_command({"op": "create", "capture": pending_capture, "path": project}))))

func _choose_package() -> void:
	_dialog(FileDialog.FILE_MODE_OPEN_DIR, "Select parent folder for packaged mod", [], func(path: String):
		_command({"op": "package", "path": path.path_join("remix_mod_" + str(Time.get_unix_time_from_system()).replace(".", "_"))}))

func _refresh_game_link() -> void:
	game_link_button.disabled = not state.get("open", false)
	game_link_button.text = "Game link !" if not game_link.error.is_empty() else ("Game linked" if game_link.state.get("linked", false) else "Link to game…")
	game_link_button.tooltip_text = game_link.error if not game_link.error.is_empty() else game_link.state.get("target", "Publish edits automatically to a game's rtx-remix/mods folder")

func _show_game_link() -> void:
	if not state.get("open", false): return
	inspector._finish_editing()
	await get_tree().process_frame
	viewport.set_mouse_capture(false)
	game_link.refresh_status()
	if is_instance_valid(game_link_dialog): game_link_dialog.queue_free()
	game_link_dialog = GameLinkDialog.new()
	game_link_dialog.link = game_link
	game_link_dialog.mod_name.text = String(state.path).get_file().get_basename().validate_filename()
	var dialog := game_link_dialog
	dialog.browse_requested.connect(func():
		var choose := func(path: String):
			if is_instance_valid(dialog): dialog.folder.text = path
		_dialog(FileDialog.FILE_MODE_OPEN_DIR, "Select game folder or rtx-remix/mods", [], choose, dialog))
	add_child(dialog)
	dialog.popup_centered()

func _choose_ingest() -> void:
	if not state.get("open", false) or import_running or not import_queue.is_empty() or is_instance_valid(import_dialog):
		_log("Open a project and finish any running import first.")
		return
	viewport.set_mouse_capture(false)
	_dialog(FileDialog.FILE_MODE_OPEN_FILES, "Ingest models or materials", ["*.usd,*.usda,*.usdc,*.gltf,*.glb,*.fbx,*.obj,*.png,*.jpg,*.jpeg,*.tga,*.exr,*.hdr,*.dds ; Assets"], _files_dropped)

func _files_dropped(files: PackedStringArray) -> void:
	if not state.get("open", false) or document == null:
		_log("Open or create a project before dropping assets to ingest.")
		return
	if import_running or not import_queue.is_empty() or is_instance_valid(import_dialog):
		_log("Finish or cancel the current import before dropping more files.")
		return
	for child in find_children("*", "Window", true, false):
		if child.visible and child.exclusive:
			_log("Close the current dialog before dropping assets.")
			return
	var accepted := PackedStringArray()
	var skipped := PackedStringArray()
	for file in files:
		var path := file.simplify_path()
		var extension := path.get_extension().to_lower()
		if not FileAccess.file_exists(path) or not (extension in MODEL_EXTENSIONS or extension in IMAGE_EXTENSIONS):
			skipped.append(path.get_file())
		elif not accepted.has(path):
			accepted.append(path)
	if not skipped.is_empty():
		_log("Skipped unsupported files, folders, or missing files: " + ", ".join(skipped))
	if accepted.is_empty():
		if not skipped.is_empty():
			var notice := AcceptDialog.new()
			notice.title = "No assets to ingest"
			notice.dialog_text = "Drop USD, glTF, GLB, FBX, OBJ, or supported image files.\nFolders and unsupported or missing files were skipped."
			add_child(notice)
			notice.confirmed.connect(notice.queue_free)
			notice.canceled.connect(notice.queue_free)
			notice.popup_centered()
		return
	viewport.set_mouse_capture(false)
	_show_editor()
	_import_options(accepted, skipped)

func _available_import_name(value: String, reserved: Array[String] = []) -> String:
	# Match USD's ASCII identifier sanitization, including UTF-8 byte replacement.
	var stem := ""
	for byte in value.to_utf8_buffer():
		var valid := (byte >= 65 and byte <= 90) or (byte >= 97 and byte <= 122) or byte == 95 or (not stem.is_empty() and byte >= 48 and byte <= 57)
		stem += String.chr(byte) if valid else "_"
	if stem.is_empty(): stem = "asset"
	var name := stem
	var index := 2
	var directory := String(state.path).get_base_dir().path_join("assets")
	while name in reserved or FileAccess.file_exists(directory.path_join(name)) or DirAccess.dir_exists_absolute(directory.path_join(name)):
		name = stem + "_" + str(index)
		index += 1
	return name

func _cancel_import() -> void:
	import_queue.clear()
	if is_instance_valid(import_dialog):
		import_dialog.hide()
		import_dialog.queue_free()
		import_dialog = null
	if document != null and import_running:
		document.cancel_ingestion()
	else:
		import_button.text = "Ingest asset"
		import_button.disabled = false

func _start_next_import() -> void:
	if import_running: return
	import_button.text = "Ingest asset"
	import_button.disabled = false
	if state.get("path", "") != import_workfile:
		import_queue.clear()
		return
	while not import_queue.is_empty():
		var request: Dictionary = import_queue.pop_front()
		var response = JSON.parse_string(document.start_ingestion(JSON.stringify(request)))
		if response.get("ok", false):
			import_running = true
			import_button.text = "Ingesting %d/%d" % [import_total - import_queue.size(), import_total]
			import_button.disabled = true
			_log("Ingesting %s (%d/%d)…" % [String(request.source).get_file(), import_total - import_queue.size(), import_total])
			return
		_log(response.get("error", "Could not start ingestion"))

func _import_options(paths: PackedStringArray, skipped := PackedStringArray()) -> void:
	var project := String(state.path)
	var folder: String = assets.current_folder
	var has_models := false
	var has_images := false
	for path in paths:
		if path.get_extension().to_lower() in IMAGE_EXTENSIONS: has_images = true
		else: has_models = true
	var dialog := ConfirmationDialog.new()
	import_dialog = dialog
	dialog.title = "Ingest " + paths[0].get_file() if paths.size() == 1 else "Ingest %d assets" % paths.size()
	dialog.ok_button_text = "Ingest"
	var fields := VBoxContainer.new()
	dialog.add_child(fields)
	_label("Destination: " + assets.catalog.folder_path(folder), fields)
	if not skipped.is_empty(): _label("Skipped %d unsupported or missing item(s)." % skipped.size(), fields)
	var asset_name := LineEdit.new()
	asset_name.text = _available_import_name(paths[0].get_file().get_basename())
	if paths.size() == 1:
		_label("Asset name", fields)
		fields.add_child(asset_name)
	else:
		_label("Files keep their names; existing names receive a numbered suffix.", fields)
		dialog.add_child(asset_name); asset_name.hide()
	var scale_field := SpinBox.new()
	scale_field.min_value = 0.0001; scale_field.max_value = 100000; scale_field.step = 0.01; scale_field.value = 1
	if has_models:
		_label("Scale multiplier (after source unit conversion)", fields)
		fields.add_child(scale_field)
	var material_kind := OptionButton.new()
	var texture_role := OptionButton.new()
	if has_images:
		_label("Image material", fields)
		for name in ["Opaque", "Cutout", "Glass"]:
			material_kind.add_item(name)
		fields.add_child(material_kind)
		_label("Texture role", fields)
		for name in ["Base color", "Normal", "Roughness", "Metallic", "Emissive"]:
			texture_role.add_item(name)
		fields.add_child(texture_role)
	_label("Normal map convention", fields)
	var normal := OptionButton.new()
	normal.add_item("OpenGL (+Y)")
	normal.add_item("DirectX (-Y)")
	fields.add_child(normal)
	dialog.confirmed.connect(func():
		if state.get("path", "") != project: return
		var names: Array[String] = []
		for path in paths:
			var is_material := path.get_extension().to_lower() in IMAGE_EXTENSIONS
			var name := _available_import_name(asset_name.text if paths.size() == 1 else path.get_file().get_basename(), names)
			names.append(name)
			var request := {"source": path, "name": name, "scale": scale_field.value, "material": is_material,
				"normal_encoding": "normal_ogl" if normal.selected == 0 else "normal_dx", "mdl_directory": root_directory.path_join("app/materials")}
			if is_material:
				request["texture_role"] = ["albedo", "normal", "roughness", "metallic", "emissive"][texture_role.selected]
				request["properties"] = {"kind": "translucent" if material_kind.selected == 2 else "opaque", "alpha_test": 6 if material_kind.selected == 1 else 7}
			import_queue.append(request)
		import_folder = folder
		import_workfile = project
		import_project = project.get_base_dir()
		import_total = import_queue.size()
		import_dialog = null
		dialog.hide()
		dialog.queue_free()
		_start_next_import())
	dialog.canceled.connect(func(): import_dialog = null; dialog.hide(); dialog.queue_free())
	# Keep all controls owned, including those hidden for this asset type.
	if not has_models:
		dialog.add_child(scale_field); scale_field.hide()
	if not has_images:
		dialog.add_child(material_kind); material_kind.hide()
		dialog.add_child(texture_role); texture_role.hide()
	add_child(dialog)
	dialog.popup_centered(Vector2i(420, 340))

func _refresh_assets() -> void:
	if not state.get("open", false):
		return
	var directory := String(state.path).get_base_dir()
	if assets.project_directory != directory:
		assets.open_project(directory)

func _use_asset(op: String) -> void:
	var ids: Array = assets.selected_ids()
	if ids.is_empty() or (op != "add_mesh" and selection.is_empty()):
		_log("Select a mesh asset first." if op == "add_mesh" else "Select a scene object and an asset first.")
		return
	var id: String = ids[0]
	var kind: String = assets.catalog.entries[id].get("kind", "unknown")
	if kind == "unknown":
		_log("Wait for the asset preview to finish identifying this asset.")
		return
	if (op == "replace_material") != (kind == "material"):
		_log("Choose a material for Replace material, or a mesh for Add / Replace mesh.")
		return
	if not assets.catalog.available(id):
		_log("This asset's files are missing.")
		return
	var target := selection
	if op == "replace_material" and not inspector.selected_material.is_empty():
		target = inspector.selected_material
	library_selection = ""
	var command := {"op": op, "asset": assets.catalog.absolute_path(id)}
	if op == "add_mesh":
		command["translation"] = viewport._array(viewport.center)
	else:
		command["path"] = target
	_command(command)

func _sync_renderer() -> void:
	if renderer == null:
		return
	var active: bool = editor_page.visible and viewport.show_render and state.get("open", false) and not last_scene.is_empty()
	renderer.set_process(active)
	if active and DisplayServer.get_name() != "headless":
		_start_renderer()

func _select_renderer(index: int) -> void:
	var selected := "openremix" if index == 1 else "remix"
	if selected == render_settings.renderer: return
	if not render_settings.select_renderer(selected):
		renderer_select.select(1 if render_settings.renderer == "openremix" else 0)
		return
	if is_instance_valid(runtime_dialog): runtime_dialog.queue_free(); runtime_dialog = null
	if is_instance_valid(render_settings_dialog): render_settings_dialog.hide()
	renderer.stop_worker()
	viewport.rendered_texture = null
	_sync_renderer()

func _start_renderer() -> void:
	if renderer.is_ready or renderer.is_starting or is_instance_valid(runtime_dialog):
		return
	if not state.get("open", false):
		_log("Open a scene before starting the renderer.")
		return
	var active := render_settings.renderer
	var configured := OS.get_environment("OPENREMIX_RUNTIME_PATH" if active == "openremix" else "REMIX_RUNTIME_PATH")
	if not runtime_paths.has(active):
		var path_file := root_directory.path_join("build/runtime_paths.json")
		if FileAccess.file_exists(path_file):
			var paths = JSON.parse_string(FileAccess.get_file_as_string(path_file))
			if paths is Dictionary and FileAccess.file_exists(str(paths.get(active, ""))): runtime_paths[active] = str(paths[active])
	var remembered: String = runtime_paths.get(active, runtime_path if active == "remix" else "")
	var built := root_directory.path_join(".work/build/openremix/src/native/libremix.so" if active == "openremix" else ".work/build/remix/src/linux/libremix.so")
	if not configured.is_empty():
		_launch_renderer(configured)
	elif not remembered.is_empty():
		_launch_renderer(remembered)
	elif FileAccess.file_exists(built):
		_launch_renderer(built)
	else:
		runtime_dialog = _dialog(FileDialog.FILE_MODE_OPEN_FILE, "Select %s runtime library" % active.capitalize(), [], _launch_renderer)

func _launch_renderer(library: String) -> void:
	runtime_paths[render_settings.renderer] = library
	if render_settings.renderer == "remix": runtime_path = library
	if not state.get("open", false) or not viewport.show_render or not editor_page.visible:
		return
	var worker := OS.get_environment("REMIX_RENDERER_WORKER")
	if worker.is_empty():
		worker = root_directory.path_join("build/remix-renderer" + (".exe" if OS.get_name() == "Windows" else ""))
	renderer.start_worker(worker, library, render_settings.values, render_settings.renderer)
	if is_instance_valid(render_settings_dialog): render_settings_dialog.set_capabilities({})

func _show_render_settings() -> void:
	viewport.set_mouse_capture(false)
	if not is_instance_valid(render_settings_dialog):
		render_settings_dialog = RenderSettingsDialog.new()
		add_child(render_settings_dialog)
		render_settings_dialog.settings_applied.connect(_apply_render_settings)
	render_settings_dialog.show_settings(render_settings.values, renderer.capabilities, render_settings.renderer, renderer.effective_settings)

func _apply_render_settings(values: Dictionary) -> void:
	if not render_settings.save_settings(values):
		if is_instance_valid(render_settings_dialog):
			render_settings_dialog.notice.text = "Could not save render settings. Check that your settings folder is writable."
		return
	if is_instance_valid(render_settings_dialog): render_settings_dialog.hide()
	var pixels := Vector2i(render_settings.values.width, render_settings.values.height)
	if not renderer.process.is_empty():
		if renderer.output_size != pixels:
			# Shared Vulkan images have a fixed extent. Reconnect the worker and
			# import a new image; the document and camera stay in the editor.
			renderer.stop_worker()
		else:
			renderer.send({"op": "render_settings", "settings": render_settings.values.duplicate(true)})
	_sync_renderer()

func _renderer_failed(message: String) -> void:
	if DisplayServer.get_name() == "headless":
		return
	if not is_instance_valid(renderer_alert):
		renderer_alert = AcceptDialog.new()
		renderer_alert.title = "Renderer stopped"
		add_child(renderer_alert)
	renderer_alert.dialog_text = message + "\n\nThe Layout view stays available. Renderer log: " + renderer.log_path
	if not renderer_alert.visible:
		renderer_alert.popup_centered()

func _renderer_response(response: Dictionary) -> void:
	if response.get("ok", false) and response.get("request_op") == "frame" and is_instance_valid(render_settings_dialog) and render_settings_dialog.visible:
		render_settings_dialog.set_effective_settings(renderer.effective_settings)
	if response.get("ok", false) and response.get("request_op") == "start":
		if is_instance_valid(render_settings_dialog): render_settings_dialog.set_capabilities(renderer.capabilities)
		if last_scene.is_empty():
			renderer.stop_worker()
			return
		renderer.send({"op": "scene", "scene": last_scene})
		renderer.send({"op": "camera", "camera": viewport.camera()})

func _process(_delta: float) -> void:
	if import_running and document != null:
		var response: String = document.poll_ingestion()
		if not response.is_empty():
			import_running = false
			var result = JSON.parse_string(response)
			if result.get("ok", false):
				_log("Asset ingested: " + result.result.asset)
				for warning in result.result.get("warnings", []):
					_log(warning)
				if assets.project_directory == import_project:
					assets.register_ingestion(result.result.asset, import_folder)
				else:
					# An already-completed job may finish after switching projects.
					# Preserve its destination folder in the original catalog.
					var catalog := AssetBrowser.Catalog.new()
					catalog.error.connect(_log)
					catalog.open_project(import_project)
					var id: String = String(result.result.asset).trim_prefix(import_project.trim_suffix("/") + "/")
					catalog.move_assets([id], import_folder if catalog.folders.has(import_folder) else "")
			else:
				_log(result.get("error", "Ingestion failed"))
			_start_next_import()

func _unhandled_key_input(event: InputEvent) -> void:
	if welcome.visible or not viewport.parent_pick_path.is_empty():
		return
	if not event.is_pressed() or event.is_echo() or get_viewport().gui_get_focus_owner() is LineEdit or get_viewport().gui_get_focus_owner() is TextEdit:
		return
	if event is InputEventKey:
		if event.is_command_or_control_pressed():
			if event.keycode == KEY_T and not event.alt_pressed and not event.shift_pressed:
				_place_selection_at_cursor()
				get_viewport().set_input_as_handled()
			elif event.keycode == KEY_S:
				_save_from_ui()
			elif event.keycode == KEY_Z:
				_command({"op": "redo" if event.shift_pressed else "undo"})
		elif event.keycode == KEY_F:
			viewport.frame_selection()

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST and not close_pending:
		close_pending = true
		viewport.set_mouse_capture(false)
		_guard_unsaved(_finish_close, func(): close_pending = false)

func _show_welcome() -> void:
	viewport.set_mouse_capture(false)
	editor_page.hide(); welcome.show()
	welcome.refresh(sessions, state.get("open", false))
	if renderer != null:
		renderer.set_process(false)

func _show_editor() -> void:
	welcome.hide(); editor_page.show()
	viewport._sync_view()
	_sync_renderer()

func _save_from_ui() -> void:
	if save_pending:
		return
	save_pending = true
	inspector._finish_editing()
	await get_tree().process_frame
	_command({"op": "save"})
	save_pending = false

func _persist_camera(saved_at := 0.0) -> bool:
	if state.get("open", false):
		return sessions.save_view(state.path, viewport.view_state(), saved_at, state.get("active_capture", ""))
	return true

func _save_view_and_preview() -> void:
	var saved_at := Time.get_unix_time_from_system()
	if not _persist_camera(saved_at):
		return
	if DisplayServer.get_name() == "headless":
		return
	var path: String = state.path
	var serial: int = save_serials.get(path, 0) + 1
	save_serials[path] = serial
	var image: Image
	if viewport.show_render and renderer.is_ready and renderer.presented_context.get("revision", -1) == state.revision and renderer.presented_context.get("camera", {}) == viewport.camera() and viewport.rendered_texture != null:
		image = viewport.rendered_texture.get_image()
	var thumbnail_view: SubViewport
	if image == null or image.is_empty():
		thumbnail_view = viewport.create_thumbnail_view()
		add_child(thumbnail_view)
	preview_jobs += 1
	_complete_preview(path, saved_at, serial, image, thumbnail_view)

func _complete_preview(path: String, saved_at: float, serial: int, image: Image, thumbnail_view: SubViewport) -> void:
	if thumbnail_view != null:
		for i in range(2):
			await RenderingServer.frame_post_draw
		image = thumbnail_view.get_texture().get_image()
		thumbnail_view.queue_free()
	if save_serials.get(path) == serial and image != null and not image.is_empty():
		var factor := minf(384.0 / image.get_width(), 216.0 / image.get_height())
		image.convert(Image.FORMAT_RGBA8)
		image.resize(maxi(1, int(image.get_width() * factor)), maxi(1, int(image.get_height() * factor)))
		var fitted := Image.create(384, 216, false, Image.FORMAT_RGBA8)
		fitted.fill(Color("111820"))
		fitted.blend_rect(image, Rect2i(Vector2i.ZERO, image.get_size()), (fitted.get_size() - image.get_size()) / 2)
		sessions.save_thumbnail(path, fitted, saved_at)
	preview_jobs -= 1
	if welcome.visible:
		welcome.refresh(sessions, state.get("open", false))

func _prepare_close() -> void:
	_cancel_import()
	_persist_camera()
	game_link.flush()
	var deadline := Time.get_ticks_msec() + 2000
	while preview_jobs > 0 and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame

func _finish_close() -> void:
	await _prepare_close()
	get_tree().quit()
