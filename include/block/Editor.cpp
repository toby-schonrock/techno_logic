#include "Editor.hpp"
#include <SFML/Window/Event.hpp>
#include <imgui.h>

ObjAtCoordVar Editor::whatIsAtCoord(const sf::Vector2i& coord) const {
    // check for nodes
    for (const auto& node: block.nodes) {
        if (coord == node.obj.pos) return node.ind;
    }
    // check connections
    std::vector<Connection> cons;
    for (const auto& net: block.conNet.nets) {
        for (const auto& portPair: net.obj.getMap()) {
            Connection con(portPair.first, portPair.second);
            if (block.collisionCheck(con, coord)) cons.push_back(con);
        }
    }
    if (cons.size() > 2)
        throw std::logic_error("Should never be more than 2 connections overlapping");
    else if (cons.size() == 2) {
        return std::make_pair(cons[0], cons[0]);
    } else if (cons.size() == 1) {
        return cons[0];
    }

    return {};
}

bool Editor::checkPropEndPosLegal(const sf::Vector2i& propPos) const {
    if (conStartPos == propPos) return false;
    auto propObj = whatIsAtCoord(propPos);
    if (!isValConTarget(propObj)) {
        ImGui::SetTooltip("Target obj invalid");
        return false;
    }
    return true;
}

// Returns ref to port at location
// if there isn't one creates one according to what's currently there;
[[nodiscard]] PortRef Editor::makeNewPortRef(const ObjAtCoordVar& var, const sf::Vector2i& pos,
                                             Direction dirIntoPort) {
    switch (typeOf(var)) {
    case ObjAtCoordType::Empty: { // make new node
        std::cout << "new node made" << std::endl;
        Ref<Node> node = block.nodes.insert(Node{pos});
        return {node, static_cast<std::size_t>(reverseDir(dirIntoPort)), PortType::node};
    }
    case ObjAtCoordType::Con: { // make new node and split connection
        Ref<Node> node = block.nodes.insert(Node{pos});
        block.splitConWithNode(std::get<Connection>(var), node);
        return {node, static_cast<std::size_t>(reverseDir(dirIntoPort)), PortType::node};
    }
    case ObjAtCoordType::Port: { // return portRef
        return std::get<PortRef>(var);
    }
    case ObjAtCoordType::Node: {
        Ref<Node> node = std::get<Ref<Node>>(var);
        return {node, static_cast<std::size_t>(reverseDir(dirIntoPort)), PortType::node};
    }
    default:
        throw std::logic_error("Cannot make connection to location which isn't viable");
    }
}

sf::Vector2i Editor::snapToGrid(const sf::Vector2f& pos) const {
    return {std::clamp(static_cast<int>(std::round(pos.x)), 0, static_cast<int>(block.size - 1)),
            std::clamp(static_cast<int>(std::round(pos.y)), 0, static_cast<int>(block.size - 1))};
}

// Called every event
// Primarily responsible for excectution of actions
// E.g. create destroy objects
void Editor::event(const sf::Event& event, const sf::Vector2f& mousePos) {
    sf::Vector2i mouseGridPos = snapToGrid(mousePos);
    if (event.type == sf::Event::MouseButtonReleased &&
        event.mouseButton.button == sf::Mouse::Left) {
        auto clickedObj = whatIsAtCoord(mouseGridPos);
        switch (state) {
        case BlockState::Idle: {              // new connection started
            if (isValConTarget(clickedObj)) { // connectable
                state = BlockState::Connecting;
                // reset end... it will contain old data
                conEndPos    = conStartPos;
                conEndObjVar = conStartObjVar;
            } else { // clickedObj was a gate or block
                // TODO
            }
            break;
        }
        case BlockState::Connecting: { // connection finished
            if (!conEndLegal) break;

            PortRef startPort =
                makeNewPortRef(conStartObjVar, conStartPos, vecToDir(conStartPos - conEndPos));
            PortRef endPort =
                makeNewPortRef(conEndObjVar, conEndPos, vecToDir(conEndPos - conStartPos));

            block.conNet.insert({startPort, endPort}, conStartCloNet, conEndCloNet);
            state = BlockState::Idle;
            conEndCloNet.reset();
            break;
        }
        }
    }
}

// Called every frame
// Responsible for ensuring correct state of "con" variables according to block state and inputs
void Editor::frame(const sf::Vector2f& mousePos) {
    sf::Vector2i mouseGridPos = snapToGrid(mousePos);
    switch (state) {
    case BlockState::Idle: {
        conStartPos    = mouseGridPos;
        conStartObjVar = whatIsAtCoord(conStartPos);
        // if network component highlight network
        switch (typeOf(conStartObjVar)) { // maybe make own function
        case ObjAtCoordType::Node: {
            conStartCloNet = block.conNet.getClosNetRef(std::get<Ref<Node>>(conStartObjVar));
            break;
        }
        case ObjAtCoordType::Con: {
            conStartCloNet =
                block.conNet.getClosNetRef(std::get<Connection>(conStartObjVar).portRef1);
            break;
        }
        default:
            conStartCloNet.reset();
        }
        break;
    }
    case BlockState::Connecting: {
        sf::Vector2i diff       = mouseGridPos - conStartPos;
        sf::Vector2i newEndProp = conStartPos + snapToAxis(diff); // default
        conEndCloNet.reset();

        if (diff == sf::Vector2i{}) { // havent moved from start maybe not nescesarry anymore
            conEndLegal = false;
            break;
        }

        // work out proposed end point based on state
        switch (typeOf(conStartObjVar)) { // from 1, up to dot(diff, portDir)
        case ObjAtCoordType::Port: {
            const auto& port     = block.getPort(std::get<PortRef>(conStartObjVar));
            int         bestDist = std::clamp(dot(port.portDir, diff), 0, INT_MAX);
            newEndProp           = conStartPos + (dirToVec(port.portDir) * bestDist);
        }
        case ObjAtCoordType::Node: { // find best AVAILABLE direction
            const auto& nodeRef  = std::get<Ref<Node>>(conStartObjVar);
            const auto& node     = block.nodes[nodeRef];
            auto        bestPort = std::max_element(
                node.ports.begin(), node.ports.end(), [&](const auto& max, const auto& elem) {
                    return block.conNet.contains(PortRef(
                               nodeRef, static_cast<std::size_t>(max.portDir), PortType::node)) ||
                           dot(max.portDir, diff) < dot(elem.portDir, diff);
                });
            int bestDist = std::clamp(dot(bestPort->portDir, diff), 0, INT_MAX);
            newEndProp   = conStartPos + (dirToVec(bestPort->portDir) * bestDist);
            break;
        }
        case ObjAtCoordType::Con: {
            Direction oldConDir =
                block.getPort(std::get<Connection>(conStartObjVar).portRef1).portDir;
            Direction newConDir = swapXY(oldConDir);
            auto      dist      = dot(newConDir, diff);
            newEndProp          = conStartPos + (dirToVec(newConDir) * dist);
            break;
        }
        case ObjAtCoordType::Empty: {
            break;
        }
        default:
            throw std::logic_error("Start hover interactions not implemented yet for this object");
            break;
        }

        if (!checkPropEndPosLegal(newEndProp)) { // if new position illegal
            conEndLegal = false;
            break;
        }
        conEndLegal  = true;
        conEndPos    = newEndProp;
        conEndObjVar = whatIsAtCoord(conEndPos);
        switch (typeOf(conEndObjVar)) { // if network component find closednet
        case ObjAtCoordType::Node: {
            conEndCloNet = block.conNet.getClosNetRef(std::get<Ref<Node>>(conEndObjVar));
            break;
        }
        case ObjAtCoordType::Con: {
            conEndCloNet = block.conNet.getClosNetRef(std::get<Connection>(conEndObjVar).portRef1);
            break;
        }
        default:
            break;
        }
        if (conStartCloNet && conEndCloNet && conStartCloNet.value() == conEndCloNet.value()) {
            ImGui::SetTooltip("Connection proposes loop"); // recomendation only (for now)
        }
        break;
    }
    }
}
