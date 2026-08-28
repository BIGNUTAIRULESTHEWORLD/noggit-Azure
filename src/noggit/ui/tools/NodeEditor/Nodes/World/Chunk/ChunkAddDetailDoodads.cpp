#include "ChunkAddDetailDoodads.hpp"
#include <noggit/ui/tools/NodeEditor/Nodes/BaseNode.inl>
#include <noggit/ui/tools/NodeEditor/Nodes/DataTypes/GenericData.hpp>

#include <external/NodeEditor/include/nodes/Node>

using namespace Noggit::Ui::Tools::NodeEditor::Nodes;

ChunkAddDetailDoodads::ChunkAddDetailDoodads()
: ContextLogicNodeBase()
{
  setName("Chunk :: AddDetailDoodads");
  setCaption("Chunk :: AddDetailDoodads");
  _validation_state = NodeValidationState::Valid;
  addPortDefault<LogicData>(PortType::In, "Logic", true);
  addPortDefault<ChunkData>(PortType::In, "Chunk", true);
  addPortDefault<UnsignedIntegerData>(PortType::In, "Global Density"
  , true);
  addPort<LogicData>(PortType::Out, "Logic", true);
}

void ChunkAddDetailDoodads::compute()
{
  // this node used to carry its own unfinished port of the client's
  // detail-doodad placement; that duplicate was removed. An implementation
  // should go through Noggit::DetailDoodads (DetailDoodads.hpp), the
  // client-matching placement the ground effect preview renders with,
  // instead of rewriting the algorithm here.
  setValidationState(NodeValidationState::Error);
  setValidationMessage("Error: AddDetailDoodads is not implemented yet.");
  return;
}

NodeValidationState ChunkAddDetailDoodads::validate()
{
  if (!static_cast<ChunkData*>(_in_ports[1].in_value.lock().get()))
  {
    setValidationState(NodeValidationState::Error);
    setValidationMessage("Error: failed to evaluate chunk input.");
    return _validation_state;
  }

  return ContextLogicNodeBase::validate();
}
