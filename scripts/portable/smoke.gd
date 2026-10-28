extends SceneTree

# Run against an extracted bundle with the build checkout hidden. The test and
# tiny fixture are supplied externally and never enter the release archive.
var app: Control
var directory := OS.get_environment("REMIX_STUDIO_PORTABLE_TEST_DIR")
var failures := 0
var frames := 0
var deadline := 0
var openremix_upscaler := OS.get_environment("REMIX_STUDIO_TEST_UPSCALER")

func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		push_error(message)

func request(command: Dictionary) -> Dictionary:
	var response: Dictionary = JSON.parse_string(app.document.execute(JSON.stringify(command)))
	check(response.get("ok", false), str(response))
	return response.get("result", {})

func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	if directory.is_empty():
		push_error("REMIX_STUDIO_PORTABLE_TEST_DIR must point at an isolated test directory")
		quit(1)
		return
	app = load("res://main.tscn").instantiate()
	if OS.get_environment("REMIX_STUDIO_TEST_RENDERER") == "openremix":
		if openremix_upscaler.is_empty(): openremix_upscaler = "none"
		app.render_settings = load("res://scripts/render_settings.gd").new(directory.path_join("settings.json"))
		app.render_settings.select_renderer("openremix")
		app.render_settings.save_settings({"backend": "pathtrace", "upscaler": openremix_upscaler, "quality": "balanced", "width": 320, "height": 180})
	root.add_child(app)
	check(app.document != null, "Bundled GDExtension failed to load")
	if app.document == null:
		quit(1)
		return
	app.renderer.status_changed.connect(func(message: String): print(message))
	var project := directory.path_join("project.usda")
	request({"op": "create", "capture": directory.path_join("capture.usda"), "path": project})
	var scene := request({"op": "scene"})
	check(not scene.get("meshes", []).is_empty(), "Bundled USD schemas did not load capture geometry")
	var image := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.8, 0.2, 0.1))
	image.save_png(directory.path_join("albedo.png"))
	var started: Dictionary = JSON.parse_string(app.document.start_ingestion(JSON.stringify({
		"source": directory.path_join("albedo.png"), "material": true,
		"mdl_directory": app.root_directory.path_join("app/materials")})))
	check(started.get("ok", false), str(started))
	if started.get("ok", false):
		var result: String = app.document.poll_ingestion()
		while result.is_empty():
			await process_frame
			result = app.document.poll_ingestion()
		var imported: Dictionary = JSON.parse_string(result)
		check(imported.get("ok", false), str(imported))
	var dds := Image.new()
	check(dds.load_dds_from_buffer(FileAccess.get_file_as_bytes(directory.path_join("assets/albedo/albedo.dds"))) == OK
		and dds.is_compressed() and dds.has_mipmaps() and dds.get_format() == Image.FORMAT_BPTC_RGBA,
		"Bundled BC7 texture ingestion failed")
	check(FileAccess.file_exists(directory.path_join("assets/albedo/materials/AperturePBR_Opacity.mdl")),
		"Bundled MDL material modules were not ingested")
	var mesh_file := FileAccess.open(directory.path_join("triangle.obj"), FileAccess.WRITE)
	mesh_file.store_string("v -1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
	mesh_file.close()
	var ingested_mesh := request({"op": "ingest", "source": directory.path_join("triangle.obj"),
		"mdl_directory": app.root_directory.path_join("app/materials")})
	check(ingested_mesh.has("asset"), "Bundled Assimp mesh ingestion failed")
	request({"op": "save"})
	app._open_project(project)
	if DisplayServer.get_name() == "headless":
		print("Portable USD, extension, material ingestion and project reopen: ", "PASS" if failures == 0 else "FAIL")
		quit(0 if failures == 0 else 1)
		return
	check(not app.renderer.process.is_empty(), "Bundled Remix worker did not autostart")
	app.last_scene.materials.append({"id": "portable_emission", "albedo": [1, 0.2, 0.1], "emissive": [1, 0.2, 0.1], "emissive_intensity": 10})
	for prototype in app.last_scene.prototypes:
		for surface in prototype.surfaces:
			surface.material = "portable_emission"
	app.viewport.frame_selection(true)
	app.renderer.response_received.connect(_response)
	deadline = Time.get_ticks_msec() + 600000

func _response(response: Dictionary) -> void:
	if not response.get("ok", false):
		check(false, str(response))
		app.renderer.stop_worker()
		quit(1)
	elif response.get("request_op") == "frame":
		frames += 1
		if frames == 8:
			_capture.call_deferred()

func _capture() -> void:
	await RenderingServer.frame_post_draw
	check(app.viewport.rendered_texture != null, "Bundled Godot could not import the Remix Vulkan texture")
	if OS.get_environment("REMIX_STUDIO_TEST_RENDERER") == "openremix":
		check(app.renderer.effective_settings.get("upscaler") == openremix_upscaler,
			"Bundled OpenRemix silently fell back from requested reconstruction")
	var screenshot := root.get_texture().get_image()
	screenshot.save_png(directory.path_join("portable-viewport.png"))
	var area: Rect2 = root.get_final_transform() * app.viewport.get_global_transform_with_canvas() * app.viewport._view_rect()
	var lit := 0
	for y in range(int(area.position.y), int(area.end.y), 2):
		for x in range(int(area.position.x), int(area.end.x), 2):
			var pixel := screenshot.get_pixel(x, y)
			if maxf(pixel.r, maxf(pixel.g, pixel.b)) > 0.15:
				lit += 1
	check(lit >= 200, "Bundled viewport contains no rendered geometry")
	print("Portable GPU viewport: ", "PASS" if failures == 0 else "FAIL", "; lit pixels: ", lit,
		"; features: ", app.renderer.capabilities)
	app.renderer.stop_worker()
	await process_frame
	await process_frame
	quit(0 if failures == 0 else 1)

func _process(_delta: float) -> bool:
	if deadline > 0 and Time.get_ticks_msec() > deadline:
		push_error("Portable GPU test timed out")
		app.renderer.stop_worker()
		quit(1)
	return false
