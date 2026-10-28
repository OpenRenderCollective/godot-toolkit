extends SceneTree

var app: Control
var presented := 0
var deadline := 0
var test_directory := ""
var restarting := false
var diagnostic_views := [23, 100, 120, 0]
var diagnostic_index := 0

func _initialize() -> void:
	_run.call_deferred()

func _run() -> void:
	app = load("res://main.tscn").instantiate()
	root.add_child(app)
	if not app.renderer.process.is_empty():
		push_error("Welcome screen started a renderer without a project")
		quit(1)
		return
	if "--lighting-only" in OS.get_cmdline_user_args() or "--diagnostics" in OS.get_cmdline_user_args():
		restarting = true
	test_directory = app.root_directory.path_join(".work/integration/" + str(Time.get_ticks_usec()))
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": test_directory.path_join("project.usda")})
	if app.renderer.process.is_empty() or not app.renderer.is_starting:
		push_error("Loading a project did not automatically start Remix")
		quit(1)
		return
	var worker_pid: int = app.renderer.process.pid
	app.render_toggle.button_pressed = false
	app.render_toggle.button_pressed = true
	if app.renderer.process.pid != worker_pid:
		push_error("Toggling Remix during startup launched a duplicate worker")
		quit(1)
		return
	app._command({"op": "add_light", "path": "/RootNode/instances/inst_0123456789ABCDEF_0", "type": "SphereLight", "intensity": 50})
	app._command({"op": "transform", "path": app.selection, "translation": [0, 3, 0], "rotation": [-90, 0, 0]})
	app._command({"op": "set_light_type", "path": app.selection, "type": "SpotLight"})
	var values := {"inputs:enableColorTemperature": true, "inputs:colorTemperature": 3000,
			"inputs:exposure": 1, "inputs:shaping:cone:angle": 60, "inputs:shaping:cone:softness": 0.25, "inputs:shaping:focus": 1}
	for property in values:
		app._command({"op": "set_authoring_property", "path": app.selection, "property": property, "value": values[property]})
	if "--light-below" in OS.get_cmdline_user_args():
		app._command({"op": "transform", "path": app.selection, "translation": [0, -3, 0]})
	app.viewport.frame_selection(true)
	# Captured NaN/Inf UVs must not abort the scene load.
	var uvs: PackedVector2Array = app.last_scene.prototypes[0].surfaces[0].uvs
	uvs[0] = Vector2(NAN, uvs[0].y); uvs[1] = Vector2(uvs[1].x, INF)
	app.last_scene.prototypes[0].surfaces[0].uvs = uvs
	if not restarting:
		app.last_scene.materials.append({"id": "smoke_emission", "albedo": [1, 0.2, 0.1], "emissive": [1, 0.2, 0.1], "emissive_intensity": 10})
		for mesh in app.last_scene.meshes:
			mesh.double_sided = false
		for prototype in app.last_scene.prototypes:
			for surface in prototype.surfaces:
				surface.material = "smoke_emission"
	app.renderer.response_received.connect(_response)
	app.renderer.status_changed.connect(func(message: String): print(message))
	deadline = Time.get_ticks_msec() + 180000

func _response(response: Dictionary) -> void:
	if not response.get("ok", false):
		push_error(str(response))
		app.renderer.stop_worker()
		quit(1)
		return
	if response.get("request_op") == "start" and "--diagnostics" in OS.get_cmdline_user_args():
		app.renderer.send({"op": "settings", "settings": {"rtx.debugView.debugViewIdx": str(diagnostic_views[0])}})
	if response.get("request_op") == "frame":
		presented += 1
		# After the restart only the spot light lights the scene. Resending the
		# scene recreates its lights, which must survive the replaced ones; the
		# capture waits until temporal history no longer holds the old light.
		if restarting and presented == 4 and "--diagnostics" not in OS.get_cmdline_user_args():
			app.renderer.send({"op": "scene", "scene": app.last_scene})
		if presented == (60 if restarting and "--diagnostics" not in OS.get_cmdline_user_args() else 8):
			_capture.call_deferred()

func _capture() -> void:
	await RenderingServer.frame_post_draw
	if app.viewport.rendered_texture == null:
		push_error("GPU texture was not attached")
		quit(1)
		return
	var image := root.get_texture().get_image()
	image.save_png(test_directory.path_join("viewport.png"))
	if "--diagnostics" in OS.get_cmdline_user_args():
		image.save_png(test_directory.path_join("view_" + str(diagnostic_views[diagnostic_index]) + ".png"))
		diagnostic_index += 1
		if diagnostic_index < diagnostic_views.size():
			presented = 0
			app.renderer.send({"op": "settings", "settings": {"rtx.debugView.debugViewIdx": str(diagnostic_views[diagnostic_index])}})
		else:
			print("Diagnostic images: " + test_directory)
			app.renderer.stop_worker()
			quit(0)
		return
	var area: Rect2 = app.viewport._view_rect()
	area = root.get_final_transform() * app.viewport.get_global_transform_with_canvas() * area
	var lit_pixels := 0
	for y in range(int(area.position.y), int(area.end.y), 2):
		for x in range(int(area.position.x), int(area.end.x), 2):
			var pixel := image.get_pixel(x, y)
			if maxf(pixel.r, maxf(pixel.g, pixel.b)) > 0.15:
				lit_pixels += 1
	if lit_pixels < 200:
		push_error("Viewport contains no rendered geometry: " + str(lit_pixels) + " lit pixels")
		app.renderer.stop_worker()
		quit(1)
		return
	# Runtimes that log their light BVH builds report the light count the frame used.
	var analytic: int = app.last_scene.lights.filter(func(light: Dictionary): return light.type != "DomeLight").size()
	var built := -1
	for line in FileAccess.get_file_as_string(app.renderer.log_path).split("\n"):
		if line.contains("[LightBVH] build") and line.contains("lights="):
			built = line.get_slice("lights=", 1).get_slice(" ", 0).to_int()
	if built >= 0 and built != analytic:
		push_error("Remix uses %d of the scene's %d lights" % [built, analytic])
		app.renderer.stop_worker()
		quit(1)
		return
	if not restarting:
		var worker_pid: int = app.renderer.process.pid
		app.render_toggle.button_pressed = false
		if app.renderer.is_processing() or app.viewport.show_render:
			push_error("Disabling Remix did not pause rendering")
			quit(1)
			return
		app._open_project(app.state.path)
		if app.renderer.is_processing() or app.renderer.process.pid != worker_pid:
			push_error("Opening with Remix disabled resumed or restarted the worker")
			quit(1)
			return
		app.render_toggle.button_pressed = true
		app._open_project(app.state.path)
		if not app.renderer.is_processing() or app.renderer.process.pid != worker_pid:
			push_error("Enabling Remix or opening a project failed to reuse the worker")
			quit(1)
			return
		app._command({"op": "add_light", "path": "/RootNode/instances/inst_0123456789ABCDEF_0", "type": "SphereLight"})
		# A killed worker must not lose the document or leave an unsignaled GPU wait.
		app.renderer.stop_worker()
		if not app.state.get("dirty", false):
			push_error("Restart discarded the document")
			quit(1)
			return
		restarting = true
		presented = 0
		app.last_scene.materials.clear()
		for prototype in app.last_scene.prototypes:
			for surface in prototype.surfaces:
				surface.material = "default"
		app.render_toggle.button_pressed = false
		app.render_toggle.button_pressed = true
	else:
		print("GPU viewport passed" + ("" if "--lighting-only" in OS.get_cmdline_user_args() else " with worker restart") + ": " + test_directory)
		app.renderer.stop_worker()
		await process_frame
		await process_frame
		quit(0)

func _process(_delta: float) -> bool:
	if deadline > 0 and Time.get_ticks_msec() > deadline:
		push_error("GPU viewport test timed out after " + str(presented) + " frames")
		app.renderer.stop_worker()
		quit(1)
	return false
