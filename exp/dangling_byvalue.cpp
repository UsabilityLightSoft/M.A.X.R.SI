struct cPosition
{
	cPosition (int x, int y) : x (x), y (y) {}
	int x;
	int y;
};
struct cVehicle
{
	cPosition position{0, 0};
};
struct cModel
{
	cVehicle vehicle;
};

cVehicle& addVehicle (cModel& model, cPosition position)
{
	model.vehicle.position = position;
	return model.vehicle;
}

int main()
{
	cModel model;
	const auto& vehicle = addVehicle (model, cPosition (1, 2));
	return vehicle.position.x;
}
