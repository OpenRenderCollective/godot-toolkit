extends SceneTree

const LayoutView = preload("res://scripts/layout_view.gd")
var failures := 0
var view: Control

func check(condition: bool, message: String) -> void:
	if not condition:
		push_error(message)
		failures += 1

func settle() -> void:
	for i in range(4):
		await process_frame
	await RenderingServer.frame_post_draw

func pixels(image: Image, green := false) -> int:
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var pixel := image.get_pixel(x, y)
			if pixel.a > 0.1 and (not green or (pixel.g > pixel.r * 1.4 and pixel.g > pixel.b)):
				count += 1
	return count

func _initialize() -> void:
	run.call_deferred()

func run() -> void:
	root.size = Vector2i(960, 640)
	view = LayoutView.new()
	root.add_child(view)
	view.position = Vector2(20, 20)
	view.size = Vector2(900, 600)
	var surface := {"positions": PackedVector3Array([Vector3(-0.8,-0.8,0), Vector3(0.8,-0.8,0), Vector3(0,0.8,0)]), "indices": PackedInt32Array([0,1,2])}
	view.set_scene({"prototypes": [{"surfaces": [surface]}], "meshes": [
		{"id": "/left", "transform": [1,0,0,0,0,1,0,0,0,0,1,0,-1.1,0,0,1], "prototype": 0},
		{"id": "/right", "transform": [1,0,0,0,0,1,0,0,0,0,1,0,1.1,0,0,1], "prototype": 0}], "lights": []})
	view.yaw = 0; view.pitch = 0; view.distance = 6
	view.queue_redraw()
	await settle()
	var layout_pixels := pixels(view.wire_viewport.get_texture().get_image())
	check(layout_pixels > 300, "GPU wireframe must contain visible geometry")
	check(view._pick(view._project(Vector3(-1.1, 0, 0))) == "/left", "Layout picking must agree with the GPU camera")
	view.selected_path = "/left"
	await settle()
	check(pixels(view.wire_viewport.get_texture().get_image(), true) > 100, "Selection must color the selected GPU mesh")
	var background := Image.create(16, 9, false, Image.FORMAT_RGBA8)
	background.fill(Color.BLUE)
	var texture := ImageTexture.create_from_image(background)
	view.rendered_texture = texture
	await settle()
	# A drone look changes the pivot while keeping the eye fixed. The shared
	# camera projection must still align the selection overlay with Remix.
	view._look(Vector2(15, -8), true)
	view.center += view._basis().x * 0.1
	view.queue_redraw()
	await settle()
	var overlay_pixels := pixels(view.wire_viewport.get_texture().get_image())
	check(overlay_pixels > 100 and overlay_pixels < layout_pixels * 0.75, "Remix overlay must draw only selected geometry")
	var projected: Vector2 = view._project(Vector3(-1.1, 0, 0))
	var gpu_projected: Vector2 = view.wire_camera.unproject_position(Vector3(-1.1, 0, 0)) / root.get_stretch_transform().get_scale() + view._view_rect().position
	check(projected.distance_to(gpu_projected) < 1, "Overlay must remain aligned after letterboxing")
	var point := Vector2i(root.get_final_transform() * view.get_global_transform_with_canvas() * view._view_rect().get_center())
	check(root.get_texture().get_image().get_pixelv(point).b > 0.8, "Transparent overlay must preserve the Remix background")
	background.fill(Color.GREEN)
	texture.update(background)
	await settle()
	check(root.get_texture().get_image().get_pixelv(point).g > 0.8, "A live texture must update without rebuilding the canvas")
	view.show_render = false
	await settle()
	check(pixels(view.wire_viewport.get_texture().get_image()) >= layout_pixels * 0.9, "Switching back to layout must restore all geometry")
	view.size = Vector2(600, 400)
	await settle()
	check(view.wire_viewport.size == Vector2i(Vector2(600, 400) * root.get_stretch_transform().get_scale()), "Resizing must update the GPU viewport")
	view.set_scene({"meshes": [], "lights": []})
	await settle()
	check(pixels(view.wire_viewport.get_texture().get_image()) == 0, "Replacing the scene must clear stale geometry")
	print("Layout viewport: %d failures" % failures)
	quit(1 if failures else 0)
