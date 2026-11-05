return {
  name = "falling-stack",
  dt = 1 / 40,
  gravity = { x = 0, y = 0, z = -10 },
  solver_iterations = 20,
  bodies = {
    { id = 1, position = { z = 1 }, friction = 0.6 },
    { id = 2, position = { z = 2.1 }, friction = 0.6 },
    { id = 3, position = { z = 3.2 }, friction = 0.6 },
  },
}
