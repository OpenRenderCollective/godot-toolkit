extends SceneTree

# OpenRemix iridescence on translucent materials: a film on glass colours the
# light it reflects and passes, on a thin wall and on a body (water), and the
# renderer's Iridescence toggle turns it off. Chromaticity is compared, so
# auto exposure does not count as a change.
const Settings = preload("res://scripts/render_settings.gd")
const MATERIAL := "/RootNode/Looks/mat_0123456789ABCDEF"
const SHADER := MATERIAL + "/Shader"
var app: Control
var frames := 0
var phase := 0
var directory: String
var deadline := 0
var checking := false
var images: Array[Image] = []
const FRAME_BUDGET := 8000
const SETTLE := 120

func _initialize() -> void: run.call_deferred()

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/openremix-glass-film/%d" % Time.get_ticks_usec())
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("settings.json"))
	app.render_settings.select_renderer("openremix")
	app.render_settings.save_settings({"backend": "pathtrace", "upscaler": "none", "ray_reconstruction": false, "width": 320, "height": 180})
	root.add_child(app)
	app.renderer.response_received.connect(response)
	app.renderer.frame_presented.connect(presented)
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": directory.path_join("project.usda")})
	# A broad light overhead for the glass to reflect, and darkness behind it:
	# under a uniform sky a film's reflection and transmission add up to white.
	app._command({"op": "add_light", "type": "RectLight", "intensity": 2})
	var light: String = app.selection
	app._command({"op": "transform", "path": light, "translation": [0, 3, 0], "rotation": [-90, 0, 0]})
	for size in ["inputs:width", "inputs:height"]:
		app._command({"op": "set_authoring_property", "path": light, "property": size, "value": 20})
	app._command({"op": "set_material_model", "path": MATERIAL, "model": "translucent"})
	author("inputs:thin_walled", true)
	# Smoked glass: little of its own lit colour, so its reflection shows.
	author("inputs:transmittance_color", [0.05, 0.05, 0.05])
	app.viewport.center = Vector3.ZERO
	app.viewport.distance = 2.5
	deadline = Time.get_ticks_msec() + 600000

func author(property: String, value) -> void:
	app._command({"op": "set_authoring_property", "path": SHADER, "property": property, "value": value})

func fail(message: String) -> void:
	push_error(message); app.renderer.stop_worker(); quit(1)

func response(value: Dictionary) -> void:
	if not value.get("ok", false): fail(str(value)); return
	if value.get("request_op") == "start":
		if not app.renderer.capabilities.get("pathtrace", false): fail("Path tracing unavailable"); return
		app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.denoiser": "off", "rtx.native.pathtrace.taa": "0", "rtx.native.pathtrace.sharc": "0", "rtx.native.pathtrace.sigma": "0", "rtx.native.pathtrace.bounces": "1", "rtx.native.pathtrace.bloom": "0"}})

func presented(_context: Dictionary) -> void:
	frames += 1
	if frames >= SETTLE and not checking and (frames - SETTLE) % 15 == 0:
		checking = true
		advance.call_deferred()

# The mean colour of the frame's centre, where the glass quad lies (the
# path tracer's speckle averages out).
func mean(image: Image) -> Color:
	var sum := Color(0, 0, 0)
	var count := 0
	for y in range(image.get_height() * 2 / 5, image.get_height() * 3 / 5):
		for x in range(image.get_width() * 2 / 5, image.get_width() * 3 / 5):
			sum += image.get_pixel(x, y); count += 1
	return sum / count

# The difference of two frames' mean chromaticities (colour over its channel sum).
func difference(a: Image, b: Image) -> float:
	var p := mean(a)
	var q := mean(b)
	var ps := maxf(p.r + p.g + p.b, 1e-3)
	var qs := maxf(q.r + q.g + q.b, 1e-3)
	print("  mean colours %s / %s" % [p, q])
	return absf(p.r / ps - q.r / qs) + absf(p.g / ps - q.g / qs) + absf(p.b / ps - q.b / qs)

func lit(image: Image) -> float:
	var sum := 0.0
	for y in range(image.get_height() * 2 / 5, image.get_height() * 3 / 5):
		for x in range(image.get_width() * 2 / 5, image.get_width() * 3 / 5):
			sum += image.get_pixel(x, y).get_luminance()
	return sum / ((image.get_height() / 5) * (image.get_width() / 5))

func advance() -> void:
	await RenderingServer.frame_post_draw
	var texture: Texture2D = app.renderer.frame_texture
	if texture == null: fail("No shared image"); return
	var image := texture.get_image()
	# The first frames may precede the scene and the settled camera: wait for both.
	if phase == 0 and (frames < 600 or lit(image) < 0.08):
		if frames < FRAME_BUDGET: checking = false; return
		fail("Shared image shows no lit glass"); return
	image.save_png(directory.path_join("phase-%d.png" % phase))
	images.append(image)
	match phase:
		0:
			author("inputs:enable_thin_film", true)
			author("inputs:thin_film_thickness_constant", 450.0)
			author("openremix:iridescence:filmIor", 2.4)
		1:
			var d := difference(images[0], image)
			print("Thin glass: film changes chromaticity by %.4f" % d)
			if d < 0.015: fail("The film did not colour thin glass"); return
			app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.iridescence": "0"}})
		2:
			var d := difference(images[0], image)
			print("Thin glass, Iridescence off: %.4f from plain" % d)
			if d > 0.008: fail("Iridescence off still coloured the glass"); return
			app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.iridescence": "1"}})
			author("inputs:thin_walled", false)
		3:
			author("inputs:enable_thin_film", false)
		4:
			var d := difference(images[3], image)
			print("Glass body: film changes chromaticity by %.4f" % d)
			if d < 0.015: fail("The film did not colour a glass body"); return
			print("OpenRemix glass film passed: " + directory)
			app.renderer.stop_worker(); quit(0); return
	frames = 0; phase += 1; checking = false
	deadline = Time.get_ticks_msec() + 600000

func _process(_delta: float) -> bool:
	if deadline and Time.get_ticks_msec() > deadline: fail("OpenRemix glass film test timed out")
	return false
