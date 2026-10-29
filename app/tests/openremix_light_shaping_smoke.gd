extends SceneTree

# OpenRemix light shaping: an IES profile and a light cookie authored in the
# inspector reach the renderer and darken what lies below the light.
const Settings = preload("res://scripts/render_settings.gd")
# A batwing profile: 5% of its peak along the axis either way.
const PROFILE := "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 5 1 1 2 0 0 0\n1 1 100\n0 45 90 135 180\n0\n5 20 100 20 5\n"
var app: Control
var light := ""
var frames := 0
var phase := 0
var directory: String
var deadline := 0
var checking := false
var plain := 0.0
const FRAME_BUDGET := 8000
const SETTLE := 120

func _initialize() -> void: run.call_deferred()

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/openremix-light-shaping/%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(directory)
	var profile := FileAccess.open(directory.path_join("batwing.ies"), FileAccess.WRITE)
	profile.store_string(PROFILE); profile.close()
	var cookie := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	cookie.fill(Color(0, 0, 0, 1))
	cookie.save_png(directory.path_join("black.png"))
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("settings.json"))
	app.render_settings.select_renderer("openremix")
	app.render_settings.save_settings({"backend": "pathtrace", "upscaler": "none", "ray_reconstruction": false, "width": 320, "height": 180})
	root.add_child(app)
	app.renderer.response_received.connect(response)
	app.renderer.frame_presented.connect(presented)
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": directory.path_join("project.usda")})
	app._command({"op": "add_light", "type": "SphereLight", "intensity": 20})
	light = app.selection
	# Above the first quad, its axis (-Z) turned straight down.
	app._command({"op": "transform", "path": light, "translation": [0, 3, 0], "rotation": [-90, 0, 0]})
	# A small emitter, well above the quad.
	author("inputs:radius", 0.1)
	app.viewport.center = Vector3.ZERO
	app.viewport.distance = 2.5
	deadline = Time.get_ticks_msec() + 600000

func author(property: String, value) -> void:
	app._command({"op": "set_authoring_property", "path": light, "property": property, "value": value})

func fail(message: String) -> void:
	push_error(message); app.renderer.stop_worker(); quit(1)

func response(value: Dictionary) -> void:
	if not value.get("ok", false): fail(str(value)); return
	if value.get("request_op") == "start":
		if not app.renderer.capabilities.get("pathtrace", false): fail("Path tracing unavailable"); return
		app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.denoiser": "off", "rtx.native.pathtrace.taa": "0", "rtx.native.pathtrace.sharc": "0", "rtx.native.pathtrace.sigma": "0", "rtx.native.pathtrace.bounces": "1", "rtx.native.pathtrace.bloom": "0", "rtx.native.pathtrace.autoExposure": "0"}})

func presented(_context: Dictionary) -> void:
	frames += 1
	if frames >= SETTLE and not checking and (frames - SETTLE) % 15 == 0:
		checking = true
		advance.call_deferred()

# Mean luminance of the frame's centre (the quad below the light).
func measure(image: Image) -> float:
	var sum := 0.0
	var count := 0
	for y in range(image.get_height() * 2 / 5, image.get_height() * 3 / 5):
		for x in range(image.get_width() * 2 / 5, image.get_width() * 3 / 5):
			sum += image.get_pixel(x, y).get_luminance(); count += 1
	return sum / count

func advance() -> void:
	await RenderingServer.frame_post_draw
	var texture: Texture2D = app.renderer.frame_texture
	if texture == null: fail("No shared image"); return
	var image := texture.get_image()
	var lum := measure(image)
	if phase == 0 and lum < 0.05:
		if frames < FRAME_BUDGET: checking = false; return
		fail("Shared image contains no lit geometry"); return
	image.save_png(directory.path_join("phase-%d.png" % phase))
	print("Light shaping phase %d: mean luminance %.4f" % [phase, lum])
	if phase == 0:
		plain = lum
		author("inputs:shaping:ies:file", directory.path_join("batwing.ies"))
	elif phase == 1:
		if lum > plain * 0.5: fail("The IES profile did not shape the light (%.4f vs %.4f)" % [lum, plain]); return
		app._command({"op": "reset_authoring_property", "path": light, "property": "inputs:shaping:ies:file"})
		author("inputs:remix:cookie:texture", directory.path_join("black.png"))
	elif phase == 2:
		if lum > plain * 0.3: fail("The cookie did not mask the light (%.4f vs %.4f)" % [lum, plain]); return
		app._command({"op": "reset_authoring_property", "path": light, "property": "inputs:remix:cookie:texture"})
	elif phase == 3:
		if lum < plain * 0.7: fail("Clearing the cookie did not restore the light (%.4f vs %.4f)" % [lum, plain]); return
		print("OpenRemix light shaping passed: " + directory)
		app.renderer.stop_worker(); quit(0); return
	frames = 0; phase += 1; checking = false
	deadline = Time.get_ticks_msec() + 600000

func _process(_delta: float) -> bool:
	if deadline and Time.get_ticks_msec() > deadline: fail("OpenRemix light shaping test timed out")
	return false
