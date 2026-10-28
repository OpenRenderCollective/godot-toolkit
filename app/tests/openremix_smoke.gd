extends SceneTree

const Settings = preload("res://scripts/render_settings.gd")
var app: Control
var frames := 0
var phase := 0
var baseline: Dictionary
var directory: String
var deadline := 0
var expecting_disconnect := false
var checking := false
# The path tracer prepares its pipelines asynchronously (tens of seconds on a cold
# cache), so early frames may be empty. Keep checking before giving up.
const FRAME_BUDGET := 8000

func _initialize() -> void: run.call_deferred()

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/openremix-smoke/%d" % Time.get_ticks_usec())
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("settings.json"))
	app.render_settings.select_renderer("openremix")
	app.render_settings.save_settings({"backend": "pathtrace", "upscaler": "none", "width": 320, "height": 180})
	root.add_child(app)
	app.renderer.response_received.connect(response)
	app.renderer.frame_presented.connect(presented)
	app.renderer.status_changed.connect(func(message: String): print(message))
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": directory.path_join("project.usda")})
	app._command({"op": "add_light", "type": "SphereLight", "intensity": 1000})
	app._command({"op": "transform", "path": app.selection, "translation": [0, 3, 0]})
	app.viewport.frame_selection(true)
	baseline = {"selection": app.selection, "revision": app.state.revision, "camera": app.viewport.camera()}
	deadline = Time.get_ticks_msec() + 600000

func fail(message: String) -> void:
	push_error(message); app.renderer.stop_worker(); quit(1)

func response(value: Dictionary) -> void:
	if not value.get("ok", false):
		if expecting_disconnect:
			expecting_disconnect = false
			app._sync_renderer.call_deferred()
			return
		fail(str(value)); return
	if value.get("request_op") == "start":
		if app.render_settings.renderer == "openremix" and not app.renderer.capabilities.get("pathtrace", false): fail("Path tracing unavailable"); return
		# Deterministic fixture path; reconstruction has its own upstream GPU suite.
		if app.render_settings.renderer != "openremix" or phase >= 6: return
		app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.denoiser": "off", "rtx.native.pathtrace.taa": "0", "rtx.native.pathtrace.sharc": "0", "rtx.native.pathtrace.sigma": "0", "rtx.native.pathtrace.bounces": "1"}})

func presented(_context: Dictionary) -> void:
	frames += 1
	if frames >= 3 and not checking and (frames - 3) % 15 == 0:
		checking = true
		advance.call_deferred()

func advance() -> void:
	await RenderingServer.frame_post_draw
	var texture: Texture2D = app.renderer.frame_texture
	if texture == null: fail("No shared image"); return
	var image := texture.get_image()
	var lit := 0
	for y in image.get_height():
		for x in image.get_width():
			if image.get_pixel(x, y).get_luminance() > 0.3: lit += 1
	if lit < 8:
		if frames < FRAME_BUDGET:
			checking = false
			return
		fail("Shared image contains no lit geometry"); return
	image.save_png(directory.path_join("phase-%d.png" % phase))
	if app.selection != baseline.selection or app.state.revision != baseline.revision or app.viewport.camera() != baseline.camera or not app.state.dirty:
		fail("Restart changed document, selection, camera or unsaved edits (selection %s/%s, revision %s/%s, camera %s/%s, dirty %s)" % [app.selection, baseline.selection, app.state.revision, baseline.revision, app.viewport.camera(), baseline.camera, app.state.dirty]); return
	print("OpenRemix phase %d: %d shared frames; effective %s" % [phase, frames, app.renderer.effective_settings])
	frames = 0; phase += 1; checking = false
	# There is no Remix runtime to switch to on Windows: skip its two phases.
	if phase == 4 and OS.get_name() == "Windows": phase = 6
	deadline = Time.get_ticks_msec() + 600000
	if phase == 1:
		app._apply_render_settings({"backend": "pathtrace", "upscaler": "none", "width": 400, "height": 240})
	elif phase == 2:
		expecting_disconnect = true
		OS.kill(app.renderer.process["pid"])
	elif phase == 3:
		app._apply_render_settings({"backend": "raster", "upscaler": "none", "width": 400, "height": 240})
	elif phase == 4:
		app._select_renderer(0)
	elif phase == 5:
		app._select_renderer(1)
	elif phase == 6:
		app._apply_render_settings({"backend": "pathtrace", "upscaler": "dlss", "ray_reconstruction": true, "quality": "quality", "width": 320, "height": 180})
	elif phase == 7:
		if app.render_settings.values.upscaler != "dlss" or not app.render_settings.values.ray_reconstruction:
			fail("Effective reconstruction settings overwrote saved preferences"); return
		app._apply_render_settings({"backend": "pathtrace", "upscaler": "fsr31", "quality": "quality", "width": 320, "height": 180})
	else:
		print("OpenRemix shared viewport, resize, recovery and mode switching passed: " + directory)
		app.renderer.stop_worker(); quit(0)

func _process(_delta: float) -> bool:
	if deadline and Time.get_ticks_msec() > deadline: fail("OpenRemix GPU test timed out")
	return false
