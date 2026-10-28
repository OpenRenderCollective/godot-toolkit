extends SceneTree
const Settings = preload("res://scripts/render_settings.gd")
var app: Control
var frames := 0
var failed := false
var directory := ""
const SHADER := "/RootNode/Looks/mat_0123456789ABCDEF/Shader"

func _initialize() -> void:
	run.call_deferred()

func check(condition: bool, message: String) -> bool:
	if not condition:
		push_error(message); failed = true
	return condition

func settle() -> bool:
	var target := frames + 16
	var deadline := Time.get_ticks_msec() + 180000
	while frames < target and not failed and Time.get_ticks_msec() < deadline:
		await process_frame
	return check(not failed and frames >= target, "Alpha viewport failed to produce frames")

func sample(image: Image, x: float, y := 0.0) -> Color:
	var area: Rect2 = app.viewport._view_rect()
	var point: Vector2 = (app.viewport._project(Vector3(x, y, 0)) - area.position) / area.size * Vector2(image.get_size())
	var sum := Color(0, 0, 0, 0)
	for yy in range(-5, 6):
		for xx in range(-5, 6): sum += image.get_pixelv(Vector2i(point) + Vector2i(xx, yy))
	return sum / 121.0

func snapshot(name: String) -> Image:
	var image: Image = app.viewport.rendered_texture.get_image()
	image.save_png(directory.path_join(name + ".png"))
	return image

func set_material(values: Dictionary) -> void:
	var commands := []
	for property in values:
		commands.append({"op": "set_authoring_property", "path": SHADER, "property": "inputs:" + property, "value": values[property]})
	app._command({"op": "batch", "commands": commands})

func finish() -> void:
	app.renderer.stop_worker()
	print("Alpha GPU test: ", "FAILED" if failed else "passed", "; artifacts: ", directory)
	await process_frame; await process_frame
	quit(1 if failed else 0)

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/alpha/%d" % Time.get_ticks_usec()).simplify_path()
	DirAccess.make_dir_recursive_absolute(directory)
	DirAccess.copy_absolute(ProjectSettings.globalize_path("res://../tests/fixtures/alpha.usda"), directory.path_join("capture.usda"))
	var texture := Image.create(96, 32, false, Image.FORMAT_RGBA8)
	for y in texture.get_height():
		for x in texture.get_width(): texture.set_pixel(x, y, Color(1, 0, 0, [0.0, 0.4, 1.0][x / 32]))
	texture.generate_mipmaps(); texture.compress(Image.COMPRESS_BPTC)
	texture.save_dds(directory.path_join("alpha.dds"))
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("render.json"))
	app.render_settings.save_settings(Settings.DEFAULTS.merged({"upscaler": "taau", "quality": "native", "width": 960, "height": 540}, true))
	root.add_child(app)
	app.renderer.frame_presented.connect(func(_context: Dictionary): frames += 1)
	app.renderer.response_received.connect(func(response: Dictionary):
		if not response.get("ok", false): check(false, str(response))
		if response.get("request_op") == "start":
			# Fixed exposure and an identity tone curve keep red/blue mixture
			# checks independent of automatic exposure and hue remapping.
			app.renderer.send({"op": "settings", "settings": {"rtx.autoExposure.enabled": "False", "rtx.tonemap.tonemapOperator": "0"}}))
	app._command({"op": "create", "capture": directory.path_join("capture.usda"), "path": directory.path_join("project.usda")})
	app.viewport.center = Vector3(0, 0, 0); app.viewport.yaw = 0; app.viewport.pitch = 0; app.viewport.distance = 7
	app.viewport._sync_view()
	if not await settle(): await finish(); return
	var image := snapshot("captured")
	var hole := sample(image, -2.16); var cutout := sample(image, -1.24)
	var blend := sample(image, 0); var solid := sample(image, 1.24)
	print("Captured samples: hole=", hole, " cutout=", cutout, " blended=", blend, " opaque=", solid)
	check(hole.b > hole.r + 0.3 and cutout.r > cutout.b + 0.3, "Captured alpha test must reveal blue background through transparent texels")
	check(solid.r > solid.b + 0.3, "The same shared mesh must remain opaque in another instance")
	# The compositor approximates lighting on transparent surfaces. Require
	# partial background transmission, distinct from both a hole and a solid.
	check(blend.r > 0.05 and blend.b > cutout.b + 0.1 and blend.b < hole.b - 0.05, "Captured alpha blending must show partial background transmission")
	if failed: await finish(); return
	# Captured additive blends ask Remix to make a temporary emissive material.
	# Keep drawing across frames to catch expired material/texture references.
	var emissive_scene: Dictionary = app.last_scene.duplicate(true)
	for mesh in emissive_scene.meshes:
		if String(mesh.id).ends_with("inst_0123456789ABCDEF_1/mesh"):
			mesh.alpha_state.dstColorBlendFactor = 1
	app.renderer.send({"op": "scene", "scene": emissive_scene})
	if not await settle(): await finish(); return
	image = snapshot("captured-additive")
	check(sample(image, 0.46).r > 0.4, "Captured additive blending must produce an emissive foreground")
	# Material overrides must supersede every instance's captured alpha state.
	if failed: await finish(); return
	app._select(SHADER)
	check(app.inspector.controls[SHADER + "|inputs:blend_enabled"].widget.disabled, "Capture-driven materials must disable override controls")
	set_material({"use_legacy_alpha_state": false, "blend_enabled": false, "alpha_test_type": 4, "alpha_test_reference_value": 128})
	check(not app.inspector.controls[SHADER + "|inputs:blend_enabled"].widget.disabled, "Disabling captured state must enable authoring controls")
	if not await settle(): await finish(); return
	image = snapshot("material-cutout")
	check(sample(image, 1.24).b > sample(image, 1.24).r + 0.3, "Authored cutout must override captured opaque state")
	set_material({"alpha_test_type": 7, "blend_enabled": true, "blend_type": 0})
	if not await settle(): await finish(); return
	image = snapshot("material-blend")
	check(sample(image, -1.7).r > 0.05 and sample(image, -1.7).b > cutout.b + 0.05, "Authored blending must replace captured cutout")
	app._command({"op": "save"})
	app._open_project(directory.path_join("project.usda"))
	if not await settle(): await finish(); return
	image = snapshot("reopened")
	check(sample(image, -1.7).r > 0.05 and sample(image, -1.7).b > cutout.b + 0.05, "Saved transparency must survive project reopen")
	set_material({"use_legacy_alpha_state": true})
	app._command({"op": "undo"})
	check(not app.last_scene.materials[0].use_legacy_alpha, "Transparency changes must be undoable")
	app._command({"op": "redo"})
	if not await settle(): await finish(); return
	image = snapshot("restored-capture")
	check(sample(image, 1.24).r > sample(image, 1.24).b + 0.3, "Re-enabling captured state must restore instance-specific opacity")
	# Vertex opacity must reach the GPU and participate in captured texture ops.
	var scene: Dictionary = app.last_scene.duplicate(true)
	for mesh in scene.meshes:
		if not String(mesh.id).ends_with("inst_0123456789ABCDEF_1/mesh"): continue
		mesh.alpha_state.textureAlphaArg2Source = 2
		mesh.alpha_state.textureAlphaOperation = 3
		# Instances share prototypes; give this one its own colored copy.
		var prototype: Dictionary = scene.prototypes[int(mesh.prototype)].duplicate(true)
		for surface in prototype.surfaces:
			var colors := PackedInt32Array(); colors.resize(surface.positions.size()); colors.fill(0x00ffffff)
			surface["colors"] = colors
		mesh.prototype = scene.prototypes.size(); scene.prototypes.append(prototype)
	app.renderer.send({"op": "scene", "scene": scene})
	if not await settle(): await finish(); return
	image = snapshot("vertex-opacity")
	check(sample(image, 0.46).b > sample(image, 0.46).r + 0.3, "Zero vertex alpha must hide even fully opaque texture texels")
	await finish()
