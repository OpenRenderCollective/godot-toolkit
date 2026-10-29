extends SceneTree

var failures := 0
var app: Control
var directory := ""
const MESH := "/RootNode/instances/inst_0123456789ABCDEF_0/mesh"
const MATERIAL := "/RootNode/Looks/mat_0123456789ABCDEF"

func check(condition: bool, message: String) -> void:
	if not condition:
		push_error(message); failures += 1

func write(path: String, text: String) -> void:
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE); file.store_string(text)

func settle() -> void:
	for i in range(4):
		await process_frame

func ingest(data: Dictionary) -> Dictionary:
	var started: Dictionary = JSON.parse_string(app.document.start_ingestion(JSON.stringify(data)))
	check(started.get("ok", false), str(started))
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		var response: String = app.document.poll_ingestion()
		if not response.is_empty():
			var decoded: Dictionary = JSON.parse_string(response)
			check(decoded.get("ok", false), str(decoded))
			return decoded.get("result", {})
		await process_frame
	check(false, "Ingestion timed out")
	return {}

func wait_previews(previews: Node) -> void:
	var deadline := Time.get_ticks_msec() + 30000
	while Time.get_ticks_msec() < deadline:
		await process_frame
		if previews.queue.is_empty() and previews.active.is_empty() and not previews.rendering:
			return
	check(false, "Thumbnail queue timed out")

func visible_pixels(image: Image) -> int:
	var background := image.get_pixel(0, 0)
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var color := image.get_pixel(x, y)
			if Vector3(color.r - background.r, color.g - background.g, color.b - background.b).length() > 0.12:
				count += 1
	return count

func _initialize() -> void:
	run.call_deferred()

func run() -> void:
	root.size = Vector2i(1600, 1000)
	app = load("res://main.tscn").instantiate()
	root.add_child(app)
	app.render_toggle.button_pressed = false
	directory = app.root_directory.path_join(".work/authoring/" + str(Time.get_ticks_usec()))
	var capture: String = app.root_directory.path_join("tests/fixtures/capture.usda")
	var original := FileAccess.get_file_as_string(capture)
	app._command({"op": "create", "capture": capture, "path": directory.path_join("project.usda")})
	app._select(MESH)
	await settle()
	var inspector: Control = app.inspector
	check(inspector.controls.has(MESH + "|doubleSided"), "Mesh inspector must expose double-sided")
	check(not inspector.controls.has(MESH + "|points"), "Raw mesh attributes must be hidden")
	check(inspector.controls.has(MATERIAL + "/Shader|inputs:metallic_constant"), "Mesh inspector must resolve bound material")
	var spin: SpinBox = inspector.controls[MATERIAL + "/Shader|inputs:metallic_constant"].widget
	var revision: int = app.state.revision
	spin.get_line_edit().grab_focus()
	spin.value = 0.8
	inspector._commit_spin(MATERIAL + "/Shader|inputs:metallic_constant")
	await settle()
	check(app.state.revision == revision + 1, "Numeric gesture must create one transaction")
	check(inspector.controls[MATERIAL + "/Shader|inputs:metallic_constant"].widget == spin and spin.get_line_edit().has_focus(), "Refresh must preserve controls and keyboard focus")
	check(is_equal_approx(app.last_scene.materials[0].metallic, 0.8), "Inspector edit must reach renderer scene")
	spin.get_line_edit().release_focus()
	await settle()
	check(app.state.revision == revision + 1, "Focus exit must not duplicate a committed edit")
	inspector.controls[MATERIAL + "/Shader|inputs:metallic_constant"].reset.pressed.emit()
	await settle()
	check(is_equal_approx(app.last_scene.materials[0].metallic, 0), "Reset button must reveal capture value")
	app._command({"op": "undo"})
	check(is_equal_approx(app.last_scene.materials[0].metallic, 0.8), "Inspector reset must be undoable")
	spin.get_line_edit().grab_focus()
	spin.get_line_edit().text = "0.35"
	app._select(MESH.get_base_dir())
	await settle()
	check(is_equal_approx(app.last_scene.materials[0].metallic, 0.35), "Changing selection must commit a typed numeric edit")
	check(inspector.controls[MESH.get_base_dir() + "|visibility"].widget.disabled, "Captured instance root must not offer unauthorable controls")
	app._command({"op": "add_light", "path": MESH, "type": "SpotLight"})
	var light: String = app.selection
	check(inspector.controls.has(light + "|inputs:shaping:cone:angle"), "Spotlight inspector needs cone controls")
	app._command({"op": "set_light_type", "path": light, "type": "RectLight"})
	check(inspector.controls.has(light + "|inputs:width") and not inspector.controls.has(light + "|inputs:radius"), "Light shape controls must change with type")
	check(inspector.controls.has(light + "|inputs:shaping:ies:file") and not inspector.controls.has(light + "|inputs:remix:cookie:texture"), "Area lights take an IES profile, not a cookie")
	app._command({"op": "set_light_type", "path": light, "type": "SphereLight"})
	var up_id := light + "|inputs:remix:cookie:up"
	check(inspector.controls.has(up_id) and inspector.controls[up_id].fields.size() == 3 and inspector.controls[up_id].fields[2].value == 1, "Cookie up is an X, Y, Z control")
	inspector.controls[up_id].fields[0].value = 1; inspector.controls[up_id].fields[2].value = 0
	inspector._commit_spin(up_id)
	await settle()
	var authored: Array = inspector.request.call({"op": "inspect", "path": light}).authoring.fields.filter(func(f: Dictionary): return f.property == "inputs:remix:cookie:up")
	check(authored[0].value == [1.0, 0.0, 0.0], "Cookie up edits reach the document")
	# Converting a material's surface model shows its transmission controls.
	app._command({"op": "set_material_model", "path": MATERIAL, "model": "translucent"})
	app._select(MESH)
	check(inspector.controls.has(MATERIAL + "/Shader|inputs:thin_walled") and not inspector.controls.has(MATERIAL + "/Shader|inputs:metallic_constant"), "Translucent materials show transmission controls")
	app._command({"op": "set_material_model", "path": MATERIAL, "model": "opaque"})
	# Subsurface scattering, as Blender's panel: Radius is an X, Y, Z control.
	app._select(MESH)
	var radius_id := MATERIAL + "/Shader|openremix:subsurface:radius"
	check(inspector.controls.has(MATERIAL + "/Shader|openremix:subsurface:weight") and inspector.controls.has(radius_id) and inspector.controls[radius_id].fields.size() == 3, "Opaque materials show the Subsurface controls")
	app._select(light)
	var source := directory.path_join("source/checker.png")
	DirAccess.make_dir_recursive_absolute(source.get_base_dir())
	var checker := Image.create(32, 32, false, Image.FORMAT_RGBA8)
	for y in range(32):
		for x in range(32):
			checker.set_pixel(x, y, Color("ed573c") if (x / 8 + y / 8) % 2 == 0 else Color("358ae6"))
	checker.save_png(source)
	var material := await ingest({"source": source, "material": true, "mdl_directory": app.root_directory.path_join("app/materials")})
	var mesh := await ingest({"source": app.root_directory.path_join("tests/fixtures/replacement.usda")})
	if not material.has("asset") or not mesh.has("asset"):
		quit(1); return
	var browser: Control = app.assets
	browser.refresh()
	await settle()
	await wait_previews(browser.previews)
	await settle()
	var material_id: String = material.asset.trim_prefix(directory + "/")
	var mesh_id: String = mesh.asset.trim_prefix(directory + "/")
	check(browser.icons.has(material_id) and browser.icons.has(mesh_id), "Both material and mesh thumbnails must render")
	for id in [material_id, mesh_id]:
		if browser.icons.has(id):
			var image: Image = browser.icons[id].get_image()
			check(image.get_size() == Vector2i(256, 256) and visible_pixels(image) > 500, "Thumbnail must contain framed geometry: " + id)
			check(not browser.previews._cached(id).is_empty(), "Thumbnail cache must track dependencies")
	if browser.icons.has(material_id):
		var image: Image = browser.icons[material_id].get_image()
		var red := 0; var blue := 0
		for y in range(256):
			for x in range(256):
				var pixel := image.get_pixel(x, y)
				if pixel.r > pixel.b * 1.4: red += 1
				if pixel.b > pixel.r * 1.4 and pixel.b > 0.3: blue += 1
		check(red > 200 and blue > 200, "DDS material thumbnail must display both checker colors")
	check(browser.errors.is_empty(), "Preview errors: " + str(browser.errors))
	app._select(MESH)
	for i in range(browser.cards.item_count):
		if browser.cards.get_item_metadata(i) == material_id:
			browser.cards.select(i)
			browser._asset_selected(i, true)
	check(app.selection == MESH and app.library_selection == material_id and inspector.controls.is_empty(), "Library selection must be read-only and retain the scene target")
	var catalog_before := FileAccess.get_file_as_string(mesh.asset)
	var mod_before := FileAccess.get_file_as_string(directory.path_join("mod.usda"))
	var folder: String = browser.catalog.create_folder("Props")
	check(browser.catalog.move_assets([mesh_id], folder), "Move mesh into virtual folder")
	check(FileAccess.get_file_as_string(mesh.asset) == catalog_before and FileAccess.get_file_as_string(directory.path_join("mod.usda")) == mod_before, "Browser moves must preserve USD and library asset bytes")
	app._use_asset("replace_material")
	check(not app.last_scene.materials[0].albedo_texture.is_empty(), "Replace must use the retained scene target")
	# A fresh preview service must read the disk cache without enqueuing composition.
	var service: Node = load("res://scripts/asset_previews.gd").new()
	root.add_child(service); service.open_project(directory)
	var completed := []
	service.preview_ready.connect(func(id: String, _texture: Texture2D, _kind: String): completed.append(id))
	service.request(material_id)
	check(completed == [material_id] and service.queue.is_empty(), "Persisted thumbnail cache must be reusable")
	# Rewrite a DDS dependency at the same path; high-resolution stamps detect it.
	var dds: String = material.asset.get_base_dir().path_join("albedo.dds")
	var texture := Image.create(32, 32, false, Image.FORMAT_RGBA8); texture.fill(Color.GREEN)
	texture.generate_mipmaps(); texture.compress(Image.COMPRESS_BPTC); texture.save_dds(dds)
	check(service._cached(material_id).is_empty(), "Changed texture must invalidate thumbnail cache")
	service.request(material_id)
	await wait_previews(service)
	check(completed.size() == 2, "Changed dependency must generate a new thumbnail")
	# Cancel during composition, then render another project's asset with the same ID.
	completed.clear()
	service.request(mesh_id, true)
	await process_frame
	var other := directory.path_join("other")
	write(other.path_join(mesh_id), FileAccess.get_file_as_string(app.root_directory.path_join("tests/fixtures/replacement.usda")))
	service.open_project(other); service.request(mesh_id)
	await wait_previews(service)
	check(completed == [mesh_id], "Project change must discard stale preview completion")
	var broken_id := "assets/empty/asset.usda"
	write(other.path_join(broken_id), "#usda 1.0\n(defaultPrim = \"Empty\")\ndef Xform \"Empty\" {}\n")
	var errors := []
	service.failed.connect(func(id: String, _message: String): errors.append(id))
	service.request(broken_id); await wait_previews(service)
	check(errors == [broken_id], "Empty geometry must report a recoverable preview failure")
	write(other.path_join(broken_id), FileAccess.get_file_as_string(app.root_directory.path_join("tests/fixtures/replacement.usda")))
	service.request(broken_id, true); await wait_previews(service)
	check(completed.has(broken_id), "Retry must recover after an asset is repaired")
	check(FileAccess.get_file_as_string(capture) == original, "Authoring UI must leave capture bytes unchanged")
	app._command({"op": "package", "path": directory.path_join("package")})
	check(not FileAccess.file_exists(directory.path_join("package/assets/.library.json")) and not DirAccess.dir_exists_absolute(directory.path_join("package/assets/.thumbnails")), "Catalog and preview cache must stay out of mod packages")
	app._select(light)
	browser.search.text = "e"
	browser.refresh()
	await settle(); await wait_previews(browser.previews)
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(directory.path_join("authoring.png"))
	print("Authoring UI: %d failures; artifacts: %s" % [failures, directory])
	quit(1 if failures else 0)
