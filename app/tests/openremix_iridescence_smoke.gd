extends SceneTree

# OpenRemix iridescence: a film authored in the inspector colours the shared
# frame, and its flow animates it.
const Settings = preload("res://scripts/render_settings.gd")
const SHADER := "/RootNode/Looks/mat_0123456789ABCDEF/Shader"
var app: Control
var frames := 0
var phase := 0
var directory: String
var deadline := 0
var checking := false
var plain_chroma := 0.0
var flowing: Image
const FRAME_BUDGET := 8000
const SETTLE := 120

func _initialize() -> void: run.call_deferred()

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/openremix-iridescence/%d" % Time.get_ticks_usec())
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("settings.json"))
	app.render_settings.select_renderer("openremix")
	app.render_settings.save_settings({"backend": "pathtrace", "upscaler": "none", "width": 320, "height": 180})
	root.add_child(app)
	app.renderer.response_received.connect(response)
	app.renderer.frame_presented.connect(presented)
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": directory.path_join("project.usda")})
	app._command({"op": "add_light", "type": "SphereLight", "intensity": 50})
	app._command({"op": "transform", "path": app.selection, "translation": [0, 3, 0]})
	# Close over the first quad, so it fills the frame's centre.
	app.viewport.center = Vector3.ZERO
	app.viewport.distance = 2.5
	# A grey, smooth surface: without a film it reflects without colour.
	author("inputs:diffuse_color_constant", [0.5, 0.5, 0.5])
	author("inputs:reflection_roughness_constant", 0.15)
	author("inputs:metallic_constant", 1.0)
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

# Lit pixels of the frame's centre (the quad) and their mean chroma (max - min channel).
func measure(image: Image) -> Vector2:
	var lit := 0
	var chroma := 0.0
	for y in range(image.get_height() * 2 / 5, image.get_height() * 3 / 5):
		for x in range(image.get_width() * 2 / 5, image.get_width() * 3 / 5):
			var c := image.get_pixel(x, y)
			if c.get_luminance() > 0.05:
				lit += 1
				chroma += maxf(c.r, maxf(c.g, c.b)) - minf(c.r, minf(c.g, c.b))
	return Vector2(lit, chroma / maxf(lit, 1))

func advance() -> void:
	await RenderingServer.frame_post_draw
	var texture: Texture2D = app.renderer.frame_texture
	if texture == null: fail("No shared image"); return
	var image := texture.get_image()
	var m := measure(image)
	if m.x < 8:
		if frames < FRAME_BUDGET: checking = false; return
		fail("Shared image contains no lit geometry"); return
	image.save_png(directory.path_join("phase-%d.png" % phase))
	print("Iridescence phase %d: %d lit pixels, mean chroma %.4f" % [phase, int(m.x), m.y])
	if phase == 0:
		plain_chroma = m.y
		author("inputs:enable_thin_film", true)
		author("inputs:thin_film_thickness_constant", 450.0)
		author("openremix:iridescence:filmIor", 2.4)
	elif phase == 1:
		if m.y < plain_chroma + 0.05: fail("The film did not colour the reflection (%.4f vs %.4f)" % [m.y, plain_chroma]); return
		author("openremix:iridescence:flowAmplitude", 600.0)
		author("openremix:iridescence:flowScale", 0.5)
		author("openremix:iridescence:flowSpeed", 3.0)
	elif phase == 2:
		flowing = image
	elif phase == 3:
		var changed := 0
		for y in image.get_height():
			for x in image.get_width():
				if (image.get_pixel(x, y) - flowing.get_pixel(x, y)).get_luminance() > 0.02 or absf(image.get_pixel(x, y).h - flowing.get_pixel(x, y).h) > 0.05: changed += 1
		print("Flow changed %d pixels" % changed)
		if changed < 50: fail("The film's flow did not animate"); return
		print("OpenRemix iridescence passed: " + directory)
		app.renderer.stop_worker(); quit(0); return
	frames = 0; phase += 1; checking = false
	deadline = Time.get_ticks_msec() + 600000

func _process(_delta: float) -> bool:
	if deadline and Time.get_ticks_msec() > deadline: fail("OpenRemix iridescence test timed out")
	return false
