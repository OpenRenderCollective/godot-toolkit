extends SceneTree

# OpenRemix subsurface scattering: a hard-edged spotlight on the fixture quad.
# With subsurface scattering, light bleeds past the cone's edge into the dark
# (red furthest, as its radius is largest); the renderer's setting turns it off.
const Settings = preload("res://scripts/render_settings.gd")
const SHADER := "/RootNode/Looks/mat_0123456789ABCDEF/Shader"
var app: Control
var light := ""
var frames := 0
var phase := 0
var directory: String
var deadline := 0
var checking := false
var plain: Image
const FRAME_BUDGET := 8000
const SETTLE := 120

func _initialize() -> void: run.call_deferred()

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/openremix-subsurface/%d" % Time.get_ticks_usec())
	app = load("res://main.tscn").instantiate()
	app.render_settings = Settings.new(directory.path_join("settings.json"))
	app.render_settings.select_renderer("openremix")
	app.render_settings.save_settings({"backend": "pathtrace", "upscaler": "none", "ray_reconstruction": false, "width": 320, "height": 180})
	root.add_child(app)
	app.renderer.response_received.connect(response)
	app.renderer.frame_presented.connect(presented)
	app._command({"op": "create", "capture": app.root_directory.path_join("tests/fixtures/capture.usda"), "path": directory.path_join("project.usda")})
	app._command({"op": "add_light", "type": "SpotLight", "intensity": 60})
	light = app.selection
	# Straight down onto the quad's centre, a small disk with a hard edge.
	app._command({"op": "transform", "path": light, "translation": [0, 3, 0], "rotation": [-90, 0, 0]})
	for property in [["inputs:radius", 0.05], ["inputs:shaping:cone:angle", 12.0], ["inputs:shaping:cone:softness", 0.0]]:
		app._command({"op": "set_authoring_property", "path": light, "property": property[0], "value": property[1]})
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
		app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.denoiser": "off", "rtx.native.pathtrace.taa": "0", "rtx.native.pathtrace.sharc": "0", "rtx.native.pathtrace.sigma": "0", "rtx.native.pathtrace.bounces": "0", "rtx.native.pathtrace.bloom": "0", "rtx.native.pathtrace.autoExposure": "0"}})

func presented(_context: Dictionary) -> void:
	frames += 1
	if frames >= SETTLE and not checking and (frames - SETTLE) % 15 == 0:
		checking = true
		advance.call_deferred()

# The light gained where the plain frame is dark (past the cone's edge, and
# the background), and how much of it is redder than blue.
func bleed(image: Image) -> Vector2:
	var light_sum := 0.0
	var red := 0.0
	for y in image.get_height():
		for x in image.get_width():
			var p := plain.get_pixel(x, y)
			if p.get_luminance() >= 0.06:
				continue
			var c := image.get_pixel(x, y)
			light_sum += maxf(c.get_luminance() - p.get_luminance(), 0); red += (c.r - p.r) - (c.b - p.b)
	return Vector2(light_sum, red)

# Whether the quad is drawn: where the plain frame shows its unlit part, the
# frame is not the brighter background (an edit can present frames of the
# scene being rebuilt, without it).
func quad_missing(image: Image) -> bool:
	var dark := 0
	var background := 0
	for y in image.get_height():
		for x in image.get_width():
			if plain.get_pixel(x, y).get_luminance() < 0.02:
				dark += 1
				if image.get_pixel(x, y).get_luminance() > 0.15: background += 1
	return background > dark / 3

# The brightest pixel: the lit disk, once the scene renders.
func brightest(image: Image) -> float:
	var top := 0.0
	for y in image.get_height():
		for x in image.get_width():
			top = maxf(top, image.get_pixel(x, y).get_luminance())
	return top

func advance() -> void:
	await RenderingServer.frame_post_draw
	var texture: Texture2D = app.renderer.frame_texture
	if texture == null: fail("No shared image"); return
	var image := texture.get_image()
	# The first frames may precede the scene and the settled camera, and an edit
	# may present a frame of the scene being rebuilt: wait for the lit disk.
	if phase > 0 and quad_missing(image): print("waiting for the quad (frame %d)" % frames)
	if (phase == 0 and frames < 600) or brightest(image) < 0.1 or (phase > 0 and quad_missing(image)):
		if frames < FRAME_BUDGET: checking = false; return
		fail("Shared image shows no lit quad"); return
	image.save_png(directory.path_join("phase-%d.png" % phase))
	match phase:
		0:
			plain = image
			author("openremix:subsurface:weight", 1.0)
			author("openremix:subsurface:radius", [1.0, 0.2, 0.1])
			author("openremix:subsurface:scale", 0.1)
		1:
			var b := bleed(image)
			print("Subsurface: %.2f light past the cone's edge, red over blue %.2f" % [b.x, b.y])
			# Sampling noise gains light past the edge too, but not redder light.
			if b.x < 2: fail("Subsurface scattering did not carry light past the edge"); return
			if b.y < 5: fail("The scattered light is not reddest"); return
			plain.set_meta("red", b.y)
			app.renderer.send({"op": "settings", "settings": {"rtx.native.pathtrace.subsurface": "0"}})
		2:
			var b := bleed(image)
			print("Subsurface off: %.2f light past the cone's edge, red over blue %.2f" % [b.x, b.y])
			if b.y > float(plain.get_meta("red")) * 0.3: fail("Subsurface off still carried red light past the edge"); return
			print("OpenRemix subsurface passed: " + directory)
			app.renderer.stop_worker(); quit(0); return
	frames = 0; phase += 1; checking = false
	deadline = Time.get_ticks_msec() + 600000

func _process(_delta: float) -> bool:
	if deadline and Time.get_ticks_msec() > deadline: fail("OpenRemix subsurface test timed out")
	return false
