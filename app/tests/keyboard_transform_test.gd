extends SceneTree

const LayoutView = preload("res://scripts/layout_view.gd")
const Gizmo = preload("res://scripts/transform_gizmo.gd")
const Kind = preload("res://scripts/keyboard_transform.gd").Kind
var view: Control
var failures := 0
var commits: Array = []

func check(condition: bool, message: String) -> void:
	if not condition: push_error(message); failures += 1

func key(code: Key) -> void:
	var event := InputEventKey.new(); event.keycode = code; event.pressed = true
	view._input(event)

func number(text: String, keypad := false) -> void:
	for character in text:
		if character == "-": key(KEY_KP_SUBTRACT if keypad else KEY_MINUS)
		elif character == ".": key(KEY_KP_PERIOD if keypad else KEY_PERIOD)
		else: key((KEY_KP_0 if keypad else KEY_0) + int(character))

func motion(position: Vector2) -> void:
	var event := InputEventMouseMotion.new(); event.position = view.get_global_transform_with_canvas() * position
	view._input(event)

func target(pose: Transform3D, light := true) -> void:
	view.gizmo.set_target({"path": "/Light", "world_transform": Gizmo.to_array(pose), "authoring": {"editable": true, "kind": "light" if light else "mesh"}})

func _initialize() -> void: run.call_deferred()

func run() -> void:
	root.size = Vector2i(1000, 800)
	view = LayoutView.new(); root.add_child(view); view.position = Vector2(50, 40); view.size = Vector2(900, 700)
	view.transform_committed.connect(func(path: String, matrix: Array): commits.append([path, Gizmo.from_array(matrix)]))
	await process_frame
	for up in [Vector3.UP, Vector3.BACK]:
		view.world_up = up; view.yaw = 0.6; view.pitch = 0.3; view.distance = 12; view.center = Vector3.ZERO
		view.rendered_texture = ImageTexture.create_from_image(Image.create(960, 540, false, Image.FORMAT_RGBA8))
		var pose := Transform3D(Basis(Vector3.UP, 0.4) * Basis.from_scale(Vector3(2, 3, 4)), Vector3(0.2, 0.1, 0.3))
		var pivot: Vector2 = view._project(pose.origin)
		var start := pivot + Vector2(80, 0)
		for axis in range(3):
			target(pose)
			check(view.gizmo.begin_modal(Kind.MOVE, start), "G must begin a preview")
			key([KEY_X, KEY_Y, KEY_Z][axis]); motion(start + Vector2(50, -35))
			var delta: Vector3 = view.gizmo.world.origin - pose.origin
			check(view.gizmo.axis == axis and not view.mouse_captured and absf(delta[axis]) > 0.1, "Axis locking must move along the chosen axis without capturing the mouse")
			delta[axis] = 0
			check(delta.is_zero_approx() and view.gizmo.world.basis.is_equal_approx(pose.basis), "Constrained moves must retain other coordinates, rotation and scale")
			key(KEY_ESCAPE)
			check(view.gizmo.world.is_equal_approx(pose) and commits.is_empty(), "Escape must restore the initial pose without an edit")
			target(pose); view.gizmo.begin_modal(Kind.ROTATE, start)
			key([KEY_X, KEY_Y, KEY_Z][axis]); motion(pivot + Vector2(0, -80))
			var rotation: Basis = view.gizmo.world.basis * pose.basis.inverse()
			check((rotation * Gizmo.AXES[axis]).is_equal_approx(Gizmo.AXES[axis]), "R must rotate around the locked world axis")
			check(is_equal_approx(rotation.get_rotation_quaternion().get_angle(), PI / 2), "A quarter-turn mouse arc must rotate by 90 degrees")
			check(view.gizmo.world.origin.is_equal_approx(pose.origin) and is_equal_approx(rotation.determinant(), 1), "Rotation must preserve translation and scale")
			key(KEY_ESCAPE)
			# Typed degrees are relative to the original pose, with a camera-
			# independent sign and no snap rounding, even after a mouse preview.
			target(pose); view.gizmo.begin_modal(Kind.ROTATE, start)
			key([KEY_X, KEY_Y, KEY_Z][axis]); motion(pivot + Vector2(0, -80))
			view.snap = true; number("-22.5", axis == 1)
			var numeric := Basis(Gizmo.AXES[axis], deg_to_rad(-22.5)) * pose.basis
			check(view.gizmo.world.basis.is_equal_approx(numeric) and view.gizmo.world.origin == pose.origin, "Typed degrees must set an exact signed rotation and preserve position/scale")
			motion(pivot + Vector2(-80, 0))
			check(view.gizmo.world.basis.is_equal_approx(numeric), "Mouse motion must not overwrite typed degrees")
			key(KEY_BACKSPACE); number("75")
			check(view.gizmo.world.basis.is_equal_approx(Basis(Gizmo.AXES[axis], deg_to_rad(-22.75)) * pose.basis), "Backspace must allow correcting fractional degrees")
			var next := (axis + 1) % 3
			key([KEY_X, KEY_Y, KEY_Z][next])
			check(view.gizmo.world.basis.is_equal_approx(Basis(Gizmo.AXES[next], deg_to_rad(-22.75)) * pose.basis), "Changing the axis must retain the typed angle without accumulating transforms")
			check("[-22.75°]" in view.gizmo.modal.hint(), "The viewport must display the exact numeric input")
			key(KEY_ESCAPE); view.snap = false
			check(view.gizmo.world.is_equal_approx(pose) and commits.is_empty(), "Numeric cancellation must restore the original transform without an edit")
			target(pose); view.gizmo.begin_modal(Kind.SCALE, start)
			key([KEY_X, KEY_Y, KEY_Z][axis]); motion(pivot + Vector2(160, 0))
			check(view.gizmo.world.basis.is_equal_approx(Basis.from_scale(Vector3.ONE + Gizmo.AXES[axis]) * pose.basis), "S must scale along the locked world axis")
			check(view.gizmo.world.origin.is_equal_approx(pose.origin), "Scaling must retain the pivot")
			key(KEY_ESCAPE)
		target(pose); view.gizmo.begin_modal(Kind.SCALE, start); motion(pivot + Vector2(160, 0))
		check(view.gizmo.world.basis.is_equal_approx(pose.basis.scaled(Vector3.ONE * 2)), "Unconstrained S must scale uniformly")
		motion(pivot)
		check(absf(view.gizmo.world.basis.determinant()) > 0, "Crossing the scale pivot must not make the transform singular")
		key(KEY_ESCAPE)
		target(pose); view.gizmo.begin_modal(Kind.MOVE, start); motion(start + Vector2(50, -35))
		var free_move: Transform3D = view.gizmo.world
		key(KEY_X); key(KEY_Y); key(KEY_Y)
		check(view.gizmo.world.is_equal_approx(free_move), "Switching and releasing axis constraints must reuse the original preview baseline")
		view.notification(Node.NOTIFICATION_WM_WINDOW_FOCUS_OUT)
		check(not view.gizmo.active and view.gizmo.world.is_equal_approx(pose), "Losing focus must cancel keyboard transforms")
		# Depth-aware aiming at an elevated mesh, with the floor behind it.
		var surface := {"positions": PackedVector3Array([Vector3(-10,-10,0), Vector3(10,-10,0), Vector3(10,10,0), Vector3(-10,10,0)]), "indices": PackedInt32Array([0,1,2,0,2,3])}
		var floor_rotation := Basis(Vector3.RIGHT, -PI / 2) if up == Vector3.UP else Basis.IDENTITY
		view.set_scene({"up_axis": "Y" if up == Vector3.UP else "Z", "prototypes": [{"surfaces": [surface]}], "meshes": [{"id": "/Ground", "transform": Gizmo.to_array(Transform3D(floor_rotation, up * 2)), "prototype": 0}]})
		view.pitch = 0.9; view.center = up * 2; view.distance = 12
		pose.origin = up * 5; target(pose)
		var destination: Vector3 = up * 2 + Vector3.RIGHT
		check(view.gizmo.begin_modal(Kind.AIM, view._project(destination)), "Shift+T must begin aiming a light")
		check(view.gizmo.modal.target.distance_to(destination) < 0.001, "Aim must target the visible surface depth rather than the camera pivot or floor")
		check((-view.gizmo.world.basis.z.normalized()).dot((destination - pose.origin).normalized()) > 0.9999, "Light local -Z must point at the cursor hit")
		check(view.gizmo.world.origin.is_equal_approx(pose.origin) and is_equal_approx(view.gizmo.world.basis.x.length(), pose.basis.x.length()), "Aiming must preserve position and physical scale")
		var aimed: Transform3D = view.gizmo.world
		motion(Vector2(-50, -50))
		check(view.gizmo.world.is_equal_approx(aimed), "Missing a surface must retain the last valid aim")
		key(KEY_ESCAPE)
		target(pose, false)
		check(not view.gizmo.begin_modal(Kind.AIM, view._project(destination)), "Shift+T must only aim lights")
	# An axis aligned with the camera and a pointer at the pivot remain usable.
	view.world_up = Vector3.UP; view.center = Vector3.ZERO; view.pitch = 0; view.yaw = 0; view.distance = 12
	target(Transform3D.IDENTITY)
	var center: Vector2 = view._project(Vector3.ZERO)
	view.gizmo.begin_modal(Kind.MOVE, center); key(KEY_Z); motion(center + Vector2(0, -50))
	check(view.gizmo.world.origin.z > 0 and is_zero_approx(view.gizmo.world.origin.x), "G Z must remain usable looking along Z")
	key(KEY_ESCAPE)
	view.gizmo.begin_modal(Kind.ROTATE, center); motion(center + Vector2(50, 0))
	check(not view.gizmo.world.basis.is_equal_approx(Basis.IDENTITY), "R started at the pivot must respond to mouse movement")
	key(KEY_ESCAPE)
	view.gizmo.begin_modal(Kind.ROTATE, center + Vector2(80, 0)); key(KEY_Z)
	key(KEY_MINUS); key(KEY_ENTER)
	check(view.gizmo.active and commits.is_empty(), "A partial minus sign must not confirm an unintended rotation")
	number("90")
	check(view.gizmo.world.basis.is_equal_approx(Basis(Vector3.BACK, -PI / 2)), "A negative right angle must use the locked axis's sign")
	for i in range(3): key(KEY_BACKSPACE)
	check(view.gizmo.modal.degrees_text.is_empty(), "Erasing all digits must return to mouse control")
	motion(center + Vector2(0, -80))
	check(view.gizmo.world.basis.is_equal_approx(Basis(Vector3.BACK, PI / 2)), "Mouse rotation must resume when numeric input is cleared")
	key(KEY_ESCAPE)
	view.gizmo.begin_modal(Kind.ROTATE, center + Vector2(80, 0)); key(KEY_X); number("450")
	check(view.gizmo.world.basis.is_equal_approx(Basis(Vector3.RIGHT, PI / 2)), "Angles beyond one revolution must preserve the requested orientation")
	key(KEY_ESCAPE)
	view.gizmo.begin_modal(Kind.SCALE, center); motion(center + Vector2(50, 0))
	check(view.gizmo.world.basis.x.length() > 1, "S started at the pivot must respond to mouse movement")
	key(KEY_ENTER)
	check(commits.size() == 1 and not view.gizmo.active, "Confirming must emit one transaction")
	view.queue_free(); await process_frame
	print("Keyboard transforms: %d failures" % failures)
	quit(1 if failures else 0)
