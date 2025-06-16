#include "transfer-pak.cpp"

Gamepad::Gamepad(Node::Port parent) {
  node = parent->append<Node::Peripheral>("Gamepad");

  port = node->append<Node::Port>("Pak");
  port->setFamily("Nintendo 64");
  port->setType("Pak");
  port->setHotSwappable(true);
  port->setAllocate([&](auto name) { return allocate(name); });
  port->setConnect([&] { return connect(); });
  port->setDisconnect([&] { return disconnect(); });
  port->setSupported({"Controller Pak", "Rumble Pak", "Transfer Pak"});

  bank = 0;

  x                = node->append<Node::Input::Axis>  ("X-Axis");
  y                = node->append<Node::Input::Axis>  ("Y-Axis");
  up               = node->append<Node::Input::Button>("Up");
  down             = node->append<Node::Input::Button>("Down");
  left             = node->append<Node::Input::Button>("Left");
  right            = node->append<Node::Input::Button>("Right");
  b                = node->append<Node::Input::Button>("B");
  a                = node->append<Node::Input::Button>("A");
  cameraUp         = node->append<Node::Input::Button>("C-Up");
  cameraDown       = node->append<Node::Input::Button>("C-Down");
  cameraLeft       = node->append<Node::Input::Button>("C-Left");
  cameraRight      = node->append<Node::Input::Button>("C-Right");
  l                = node->append<Node::Input::Button>("L");
  r                = node->append<Node::Input::Button>("R");
  z                = node->append<Node::Input::Button>("Z");
  start            = node->append<Node::Input::Button>("Start");
  rangeReducer1    = node->append<Node::Input::Button>("Range Reducer 1");
  rangeReducer2    = node->append<Node::Input::Button>("Range Reducer 2");
}

Gamepad::~Gamepad() {
  disconnect();
}

auto Gamepad::save() -> void {
  if(!slot) return;
  if(slot->name() == "Controller Pak") {
    ram.save(pak->write("save.pak"));
  }
  if(slot->name() == "Transfer Pak") {
    transferPak.save();
  }
}

auto Gamepad::allocate(string name) -> Node::Peripheral {
  if(name == "Controller Pak") return slot = port->append<Node::Peripheral>("Controller Pak");
  if(name == "Rumble Pak"    ) return slot = port->append<Node::Peripheral>("Rumble Pak");
  if(name == "Transfer Pak"  ) return slot = port->append<Node::Peripheral>("Transfer Pak");
  return {};
}

auto Gamepad::connect() -> void {
  if(!slot) return;
  if(slot->name() == "Controller Pak") {
    bool create = true;

    node->setPak(pak = platform->pak(node));
    system.controllerPakBankCount = system.configuredControllerPakBankCount; //reset controller bank count
    ram.allocate(system.controllerPakBankCount * 32_KiB); //allocate N banks * 32KiB, max # of banks allowed is 62
    bank = 0;
    formatControllerPak();
    if(auto fp = pak->read("save.pak")) {
      if(fp->attribute("loaded").boolean()) {
        //read the bank count
        u8 banks;
        u32 bank_size;

        fp->seek(0x20 + 0x1A);
        fp->read(array_span<u8>{&banks, sizeof(banks)});
        fp->seek(0);

        if (banks < 1) {
          banks = 1;
        } else if (banks > 62) {
          banks = 62;
        }

        bank_size = 32_KiB * banks;

        if (bank_size != ram.size) {
          ram.allocate(bank_size);

          //update the system controller bank count
          system.controllerPakBankCount = banks;
        }
        ram.load(pak->read("save.pak"));

        if (fp->size() != bank_size) {
          //reallocate vfs node
          pak->remove(fp);
          pak->append("save.pak", bank_size);
          ram.save(pak->write("save.pak")); //write data back to filesystem
        }

        create = false;
      }
    }

    if (create) {
      //we need to create a controller pak file, so reallocate the vfs file to configured size
      if (auto fp = pak->read("save.pak")) {
        pak->remove(fp);
        pak->append("save.pak", system.controllerPakBankCount * 32_KiB);
        ram.save(pak->write("save.pak"));
      }
    }
  }
  if(slot->name() == "Rumble Pak") {
    motor = node->append<Node::Input::Rumble>("Rumble");
  }
  if(slot->name() == "Transfer Pak") {
    transferPak.load(slot);
  }
}

auto Gamepad::disconnect() -> void {
  if(!slot) return;
  if(slot->name() == "Controller Pak") {
    save();
    ram.reset();
  }
  if(slot->name() == "Rumble Pak") {
    rumble(false);
    node->remove(motor);
    motor.reset();
  }
  if(slot->name() == "Transfer Pak") {
    transferPak.unload();
  }
  port->remove(slot);
  slot.reset();
}

auto Gamepad::rumble(bool enable) -> void {
  if(!motor) return;
  motor->setEnable(enable);
  platform->input(motor);
}

auto Gamepad::comm(n8 send, n8 recv, n8 input[], n8 output[]) -> n2 {
  b1 valid = 0;
  b1 over = 0;

  //status
  if(input[0] == 0x00 || input[0] == 0xff) {
    output[0] = 0x05;  //0x05 = gamepad; 0x02 = mouse
    output[1] = 0x00;
    output[2] = 0x02;  //0x02 = nothing present in controller slot
    if(ram || motor || (slot && slot->name() == "Transfer Pak")) {
      output[2] = 0x01;  //0x01 = pak present
    }
    valid = 1;
  }

  //read controller state
  if(input[0] == 0x01) {
    u32 data = read();
    output[0] = data >> 24;
    output[1] = data >> 16;
    output[2] = data >>  8;
    output[3] = data >>  0;
    if(recv <= 4) {
      over = 0;
    } else {
      over = 1;
    }
    valid = 1;
  }

  //read pak
  if(input[0] == 0x02 && send >= 3 && recv >= 1) {
    //controller pak
    if(ram) {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        for(u32 index : range(recv - 1)) {
          // read into current bank
          if(address <= 0x7FFF) output[index] = ram.read<Byte>(bank * 32_KiB + address);
          else output[index] = 0;
          address++;
        }
        output[recv - 1] = pif.dataCRC({&output[0], recv - 1u});
        valid = 1;
      }
    }

    //rumble pak
    if(motor) {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        for(u32 index : range(recv - 1)) {
          if(address <= 0x7FFF) output[index] = 0;
          else if(address <= 0x8FFF) output[index] = 0x80;
          else output[index] = motor->enable() ? 0xFF : 0x00;
          address++;
        }
        output[recv - 1] = pif.dataCRC({&output[0], recv - 1u});
        valid = 1;
      }
    }

    //transfer pak
    if(slot && slot->name() == "Transfer Pak") {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        for(u32 index : range(recv - 1)) output[index] = transferPak.read(address++);
        output[recv - 1] = pif.dataCRC({&output[0], recv - 1u});
        valid = 1;
      }
    }
  }

  //write pak
  if(input[0] == 0x03 && send >= 3 && recv >= 1) {
    //controller pak
    if(ram) {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        //check if address is bank switch command
        if (address == 0x8000) {
          if (send >= 4) {
            u8 reqBank = input[3];
            if (reqBank < system.controllerPakBankCount) {
              bank = reqBank;
            }
          } else {
            if (system.homebrewMode) {
              debug(unusual, "Controller Pak bank switch command with no bank specified");
            }
            bank = 0;
          }

          if (system.homebrewMode) {
            //Verify we have 32 bytes (1 block) input and each value is the same bank
            if (send == 35) {
              u8 bank = input[3];
              for (u32 i = 4; i < 35; i++) {
                if (input[i] != bank) {
                  debug(unusual, "Controller Pak bank switch command with mismatched data");
                  break;
                }
              }
            } else {
              debug(unusual, "Controller Pak bank switch command with invalid data length");
            }
          }


          output[0] = pif.dataCRC({&input[3], send - 3u});
          valid = 1;
        } else {
          for(u32 index : range(send - 3)) {
            if(address <= 0x7FFF) ram.write<Byte>(bank * 32_KiB + address, input[3 + index]);
            address++;
          }
          output[0] = pif.dataCRC({&input[3], send - 3u});
          valid = 1;
        }
      }
    }

    //rumble pak
    if(motor) {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        output[0] = pif.dataCRC({&input[3], send - 3u});
        valid = 1;
        if(address >= 0xC000) rumble(input[3] & 1);
      }
    }

    //transfer pak
    if(slot && slot->name() == "Transfer Pak") {
      u16 address = (input[1] << 8 | input[2] << 0) & ~31;
      if(pif.addressCRC(address) == (n5)input[2]) {
        for(u32 index : range(send - 3)) {
          transferPak.write(address++, input[3 + index]);
        }
        output[0] = pif.dataCRC({&input[3], send - 3u});
        valid = 1;
      }
    }
  }

  n2 status = 0;
  status.bit(0) = valid;
  status.bit(1) = over;
  return status;
}

//virtual notch snapping- placement seems best before any manipulation; after response curve appears jittery in controller tests
auto Gamepad::virtualNotch(double initialLength, double initialAngle, double outerDeadzoneInputRadiusMax) -> double {
    auto configuredNotchLengthFromEdge = 0.1; //user-defined [0.0, 1.0] (default 0.1); the default value cannot be configured by user in other projects
    auto configuredMaxNotchAngularDist = 0.0; //user-defined in degrees [0.0, 45.0] (default 0.0); values under 15.0 are likely to be more favorable
    print("configuredNotchLengthFromEdge: ", configuredNotchLengthFromEdge, "\n");
    print("configuredMaxNotchAngularDist: ", configuredMaxNotchAngularDist, "\n");
    if(configuredNotchLengthFromEdge > 0.0 && configuredMaxNotchAngularDist > 0.0) {
        auto lengthToNotchStart = (1.0 - configuredNotchLengthFromEdge) * outerDeadzoneInputRadiusMax;
        double maxNotchAngularDistRadians = configuredMaxNotchAngularDist * Math::Pi / 180.0;
        if(initialLength >= lengthToNotchStart) {
            auto angle = initialAngle + 2.0 * Math::Pi;
            auto windowedAngle = angle;
            while(windowedAngle > Math::Pi / 4.0) windowedAngle -= Math::Pi / 4.0;
            if((windowedAngle <= 0.0 + maxNotchAngularDistRadians) || (windowedAngle >= Math::Pi / 4.0 - maxNotchAngularDistRadians)) {
                angle += maxNotchAngularDistRadians;
                angle -= fmod(angle, Math::Pi / 4.0);
                return angle;
            }
        }
    }
    return initialAngle;
}

auto Gamepad::responseCurve(double lengthAbsolute, double innerDeadzoneSize, double cardinalMaximum) -> double {
    auto inflectionPointDistancePercentage = 50.0; //user-defined (innerDeadzone, cardinalMax) (default 50.0)
    print("inflectionPointDistancePercentage: ", inflectionPointDistancePercentage, "\n");
    auto inflectionPoint = inflectionPointDistancePercentage / 100.0 * (cardinalMaximum - innerDeadzoneSize) + innerDeadzoneSize;
    auto b = 1.0;  //keep for clarity or remove to reduce number of operations performed?
    auto c = b * (log(1.0 - cos(Math::Pi * (inflectionPoint - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize))) - log(2.0)) / log((inflectionPoint - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)); //c = 2.0 * log(sin(Math::Pi/2.0*(inflectionPoint-innerDeadzoneSize)/(cardinalMaximum-innerDeadzoneSize)))/log((inflectionPoint-innerDeadzoneSize)/(cardinalMaximum-innerDeadzoneSize)); more efficient but requires b = 1.0 to remove a
    auto maxA = 0.0;
    
    auto proportionalSensitivity = 1.0; //user-defined (default 1.0); Should this only apply to a linear response? What percentage range should be used? Place outside of Gamepad::responseCurve()?
    print("proportionalSensitivity: ", proportionalSensitivity, "\n");
    auto rangeReduction1 = (rangeReducer1->value()) ? 0.50 : 0.0; //user-defined (default 0.50); same questions as above; currently tied to either a button or key hold for activation; Is this best, or is switching to a toggle, cycle, or some combination better?
    print("rangeReduction1: ", rangeReduction1, "\n");
    auto rangeReduction2 = (rangeReducer2->value()) ? 0.25 : 0.0; //user-defined (default 0.25); refer to nearest above comment
    print("rangeReduction2: ", rangeReduction2, "\n");
    auto totalProportionalFactor = std::clamp(proportionalSensitivity, 0.0, 5.0) * std::clamp(1.0 - (rangeReduction1 + rangeReduction2), 0.0, 5.0); //Should these three modifiers be combined and apply to all input devices or be treated differently depending upon what devices and configurations are used?
    print("totalProportionalFactor: ", totalProportionalFactor, "\n");

    if(response == Response::Aggressive || response == Response::AggressiveToLinear || response == Response::LinearToAggressive) {
        maxA = Math::Pi * 1e-09 / (Math::Pi * 1e-09 - c * (cardinalMaximum - innerDeadzoneSize) * tan(Math::Pi * 1e-09/(2*(cardinalMaximum - innerDeadzoneSize)))); //high values of a can cause early input to approach infinity; take derivative of response function and solve for a at point (innerDeadzone + 1e-09, 0) where 1e-09 is an epsilon
        print("maxA: ", maxA, "\n");
    } else {
        maxA = 1.0;
        print("maxA: ", maxA, "\n");
    }
    auto aMultiplier = 100.0; //user-defined (0.0, 100.0] (default 100.0); used to produce a more relaxed or aggressive curve; values close to 0.0 and 100.0 create strongest response when relaxed and aggressive, respectively
    print("aMultiplier: ", aMultiplier, "\n");
    auto a = aMultiplier / 100.0 * maxA;
    print("a: ", a, "\n");

    switch(response) {
    
    case Response::Linear: {
        lengthAbsolute = totalProportionalFactor * (lengthAbsolute - innerDeadzoneSize) * cardinalMaximum / (cardinalMaximum - innerDeadzoneSize) / lengthAbsolute;
        break;
    }
    case Response::Relaxed: case Response::Aggressive: {
        lengthAbsolute = totalProportionalFactor * pow(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)), (a / b)) * cardinalMaximum * pow((sin(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)) * Math::Pi / 2.0)), (2.0 * (b - a) / c)) / lengthAbsolute;
        break;
    }
    case Response::RelaxedToLinear: case Response::AggressiveToLinear: {
        if(lengthAbsolute <= inflectionPoint) {
            lengthAbsolute = totalProportionalFactor * pow(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)), (a / b)) * cardinalMaximum * pow((sin(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)) * Math::Pi / 2.0)), (2.0 * (b - a) / c)) / lengthAbsolute;
        } else {
            lengthAbsolute = totalProportionalFactor * (lengthAbsolute - innerDeadzoneSize) * cardinalMaximum / (cardinalMaximum - innerDeadzoneSize) / lengthAbsolute;
        }
        break;
    }
    case Response::LinearToRelaxed: case Response::LinearToAggressive: {
        if(lengthAbsolute <= inflectionPoint) {
            lengthAbsolute = totalProportionalFactor * (lengthAbsolute - innerDeadzoneSize) * cardinalMaximum / (cardinalMaximum - innerDeadzoneSize) / lengthAbsolute;
        } else {
            lengthAbsolute = totalProportionalFactor * pow(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)), (a / b)) * cardinalMaximum * pow((sin(((lengthAbsolute - innerDeadzoneSize) / (cardinalMaximum - innerDeadzoneSize)) * Math::Pi / 2.0)), (2.0 * (b - a) / c)) / lengthAbsolute;
        }
        break;
    }
    }
    return lengthAbsolute;
}

auto Gamepad::read() -> n32 {
  platform->input(x);
  platform->input(y);
  platform->input(up);
  platform->input(down);
  platform->input(left);
  platform->input(right);
  platform->input(b);
  platform->input(a);
  platform->input(cameraUp);
  platform->input(cameraDown);
  platform->input(cameraLeft);
  platform->input(cameraRight);
  platform->input(l);
  platform->input(r);
  platform->input(z);
  platform->input(start);
  platform->input(rangeReducer1);
  platform->input(rangeReducer2);

  /*
  current lines intended to be configurable:
  Line 299: configuredNotchLengthFromEdge
  Line 300: configuredMaxNotchAngularDist
  Line 321: inflectionPointDistancePercentage
  Line 328: proportionalSensitivity
  Line 330: rangeReduction1
  Line 332: rangeReduction2
  Line 344: aMultiplier
  Line 416: outputStyleChoice
  Line 430: rangeMultiplierNumerator
  Line 477: innerDeadzone
  Line 478: deadzoneShape
  Line 488: notchSnappingEnabled
  Line 499: responseCurveMode
  */
  
  string outputStyleChoice = "VirtualOctagon"; //user-defined (default VirtualOctagon); refer to the immediate below choices
  if(outputStyleChoice == "CustomOctagon" ) outputStyle = OutputStyle::CustomOctagon;
  if(outputStyleChoice == "CustomCircle"  ) outputStyle = OutputStyle::CustomCircle;
  if(outputStyleChoice == "CustomMorphed" ) outputStyle = OutputStyle::CustomMorphed;
  if(outputStyleChoice == "DiagonalCircle") outputStyle = OutputStyle::DiagonalCircle;
  if(outputStyleChoice == "VirtualOctagon") outputStyle = OutputStyle::VirtualOctagon;
  if(outputStyleChoice == "MaxCircle"     ) outputStyle = OutputStyle::MaxCircle;
  if(outputStyleChoice == "CardinalCircle") outputStyle = OutputStyle::CardinalCircle;
  if(outputStyleChoice == "Morphed"       ) outputStyle = OutputStyle::Morphed;
  print("outputStyleChoice: ", outputStyleChoice, "\n\n");
 
  auto cardinalMax = 85.0; //N64 position count value for like-new controller
  auto diagonalMax = 69.0; //N64 position count value for like-new controller
  auto outerDeadzoneRadiusMax = 85.0; //radius of circle needed to cover given OutputStyle and correct input to nearest edge when beyond the radius
  auto rangeMultiplierNumerator = 85.0; //user-defined [0, 127] (default 85.0); only affects outputStyleChoice strings that include "Custom"
  auto rangeMultiplier = rangeMultiplierNumerator / cardinalMax; //for CustomOctagon, rangeMultiplierNumerator / (diagonalMax * sqrt(2.0)) could be done instead but the current works fine because of the later clamps
  print("rangeMultiplier: ", rangeMultiplierNumerator, "/", cardinalMax, "\n");
  
  switch(outputStyle) { //too messy? single-line cases seemed worse and if-else statements seemed hard to follow
  
  case OutputStyle::CustomMorphed:
      diagonalMax = rangeMultiplier * 69.0;
      outerDeadzoneRadiusMax = rangeMultiplier * 85.0;
      cardinalMax =  rangeMultiplier * 85.0;
      break;
  case OutputStyle::CustomOctagon:
      diagonalMax = rangeMultiplier * 69.0;
      outerDeadzoneRadiusMax = diagonalMax * sqrt(2.0);
      cardinalMax =  rangeMultiplier * 85.0;
      break;
  case OutputStyle::CustomCircle:
      outerDeadzoneRadiusMax = rangeMultiplier * 85.0;
      cardinalMax = rangeMultiplier * 85.0;
      break;
  case OutputStyle::VirtualOctagon:
      diagonalMax = 69.0;
      outerDeadzoneRadiusMax = diagonalMax * sqrt(2.0);
      cardinalMax = 85.0;
      break;
  case OutputStyle::DiagonalCircle:
      outerDeadzoneRadiusMax = diagonalMax * sqrt(2.0);
      cardinalMax = diagonalMax * sqrt(2.0);
      break;
  case OutputStyle::MaxCircle:
      outerDeadzoneRadiusMax = 127.0;
      cardinalMax = 127.0;
      break;
  case OutputStyle::CardinalCircle:
      outerDeadzoneRadiusMax = 85.0;
      cardinalMax = 85.0;
      break;
  case OutputStyle::Morphed:
      diagonalMax = 69.0;
      outerDeadzoneRadiusMax = 85.0;
      cardinalMax = 85.0;
      break;
  }

  print("cardinalMax: ", cardinalMax, "\n");
  print("outerDeadzoneRadiusMax: ", outerDeadzoneRadiusMax, "\n");

  auto innerDeadzone = 7.0; //user-defined [0, cardinalMax) (default 7.0); deadzone where input less than assigned value is 0
  string deadzoneShape = "Square"; //user-defined (default Square); options are Square and Circle
  print("innerDeadzone: ", innerDeadzone, "\n");
  print("deadzoneShape: ", deadzoneShape, "\n\n");

  //scale {-32767 ... +32767} to {-outerDeadzoneRadiusMax ... +outerDeadzoneRadiusMax}
  auto ax = x->value() * outerDeadzoneRadiusMax / 32767.0;
  auto ay = y->value() * outerDeadzoneRadiusMax / 32767.0;
  print("ax start value:", ax, "\n");
  print("ay start value:", ay, "\n\n");

  bool notchSnappingEnabled = false; //user-defined (default false); refer to Gamepad::virtualNotch() above (line 298) for customization
  if(notchSnappingEnabled == true) {
      auto initialLength = hypot(ax, ay);
      auto initialAngle = atan2(ay, ax);
      auto currentAngle = virtualNotch(initialLength, initialAngle, outerDeadzoneRadiusMax);
      ax = cos(currentAngle) * initialLength;
      ay = sin(currentAngle) * initialLength;
      print("ax notched: ", ax, "\n");
      print("ay notched: ", ay, "\n\n");
  }
  
  string responseCurveMode = "Linear"; //user-defined (default Linear); refer to the immediate below choices and Gamepad::responseCurve above (line 320) for further customization
  if(responseCurveMode == "Linear") response = Response::Linear;
  if(responseCurveMode == "Relaxed") response = Response::Relaxed;
  if(responseCurveMode == "Aggressive") response = Response::Aggressive;
  if(responseCurveMode == "RelaxedToLinear") response = Response::RelaxedToLinear;
  if(responseCurveMode == "LinearToRelaxed") response = Response::LinearToRelaxed;
  if(responseCurveMode == "AggressiveToLinear") response = Response::AggressiveToLinear;
  if(responseCurveMode == "LinearToAggressive") response = Response::LinearToAggressive;
  print("responseCurveMode: ", responseCurveMode, "\n\n");

  auto length = hypot(ax, ay);
  //create inner dead-zone of chosen shape in range {-innerDeadzone ... +innerDeadzone} and scale from it up to outer circular dead-zone of radius outerDeadzoneRadiusMax
      if(deadzoneShape == "Square") {
          auto lengthAbsoluteX = abs(ax);
          auto lengthAbsoluteY = abs(ay);
          if(lengthAbsoluteX <= innerDeadzone) {
              lengthAbsoluteX = 0.0;
          } else {
              lengthAbsoluteX = responseCurve(lengthAbsoluteX, innerDeadzone, cardinalMax);
          }
          ax *= lengthAbsoluteX;
          print("ax square dz post-response: ", ax, "\n\n");
          if(lengthAbsoluteY <= innerDeadzone) {
              lengthAbsoluteY = 0.0;
          } else {
              lengthAbsoluteY = responseCurve(lengthAbsoluteY, innerDeadzone, cardinalMax);
          }
          ay *= lengthAbsoluteY;
          print("ay square dz post-response: ", ay, "\n\n");
      } else if(length < innerDeadzone) {
          length = 0.0;
      } else {
          length = responseCurve(length, innerDeadzone, cardinalMax);
          ax *= length;
          ay *= length;
          print("ax circle dz post-response: ", ax, "\n");
          print("ay circle dz post-response: ", ay, "\n\n");
      }
      auto scaledLength = hypot(ax, ay);
      if (scaledLength > outerDeadzoneRadiusMax) {
      length = outerDeadzoneRadiusMax / length;
      ax *= length;
      ay *= length;
      print("ax post-response post-correction: ", ax, "\n");
      print("ay post-response post-correction: ", ay, "\n\n");
  }

  //bound diagonals to an octagonal range {-diagonalMax ... +diagonalMax} and scale only to circular edge when morphing
  if(outputStyle == OutputStyle::VirtualOctagon || outputStyle == OutputStyle::CustomOctagon || outputStyle == OutputStyle::Morphed || outputStyle == OutputStyle::CustomMorphed) {
    if(ax != 0.0 && ay != 0.0) {
        auto slope = ay / ax;
        auto edgex = copysign(cardinalMax / (abs(slope) + (cardinalMax - diagonalMax) / diagonalMax), ax);
        auto edgey = copysign(min(abs(edgex * slope), cardinalMax / (1.0 / abs(slope) + (cardinalMax - diagonalMax) / diagonalMax)), ay);
        edgex = edgey / slope;

        auto distanceToEdge = hypot(edgex, edgey);

        if(outputStyle == OutputStyle::VirtualOctagon || outputStyle == OutputStyle::CustomOctagon) {
            length = hypot(ax, ay);
            if(length > distanceToEdge) {
                ax = edgex;
                ay = edgey;
                print("ax post-VirtualOctagon: ", ax, "\n");
                print("ay post-VirtualOctagon: ", ay, "\n\n");
            }
        } else {
            auto scale = distanceToEdge / outerDeadzoneRadiusMax;
            ax *= scale;
            ay *= scale;
            print("ax post-morph: ", ax, "\n");
            print("ay post-morph: ", ay, "\n\n");
        }
    }
  }

  //keep cardinal input within positive and negative bounds of cardinalMax
  if(outputStyle == OutputStyle::VirtualOctagon || outputStyle == OutputStyle::CustomOctagon) {
    if(abs(ax) > cardinalMax) ax = copysign(cardinalMax, ax);
    if(abs(ay) > cardinalMax) ay = copysign(cardinalMax, ay);
    print("ax post-VirtualOctagon post-clamp: ", ax, "\n");
    print("ay post-VirtualOctagon post-clamp: ", ay, "\n\n");
  }
  
  //add epsilon to counteract floating point precision error
  ax = copysign(abs(ax) + 1e-09, ax);
  ay = copysign(abs(ay) + 1e-09, ay);
  print("ax post-epsilon: ", ax, "\n");
  print("ay post-epsilon: ", ay, "\n\n\n\n");

  n32 data;
  data.byte(0) = s8(-ay);
  data.byte(1) = s8(+ax);
  data.bit(16) = cameraRight->value();
  data.bit(17) = cameraLeft->value();
  data.bit(18) = cameraDown->value();
  data.bit(19) = cameraUp->value();
  data.bit(20) = r->value();
  data.bit(21) = l->value();
  data.bit(22) = 0;  //GND
  data.bit(23) = 0;  //RST
  data.bit(24) = right->value() & !left->value();
  data.bit(25) = left->value() & !right->value();
  data.bit(26) = down->value() & !up->value();
  data.bit(27) = up->value() & !down->value();
  data.bit(28) = start->value();
  data.bit(29) = z->value();
  data.bit(30) = b->value();
  data.bit(31) = a->value();
  
  //when L+R+Start are pressed: the X/Y axes are zeroed, RST is set, and Start is cleared
  if(l->value() && r->value() && start->value()) {
    data.byte(0) = 0;  //Y-Axis
    data.byte(1) = 0;  //X-Axis
    data.bit(23) = 1;  //RST
    data.bit(28) = 0;  //Start
  }

  return data;
}

auto Gamepad::getInodeChecksum(u8 bank) -> u8 {
  if (bank < 62) {
    u32 checksum = 0;
    u32 i = bank == 0 ? 3 + ram.read<Byte>(0x20 + 0x1a) * 2 : 1; //first bank has 3 + bank * 2 system pages, other banks have 127.

    for (i; i < 0x100; i++) {
      checksum += ram.read<Byte>((1 + bank) * 0x100) + ram.read<Byte>((1 + bank) * 0x100 + 0x01);
    }

    return checksum;
  }

  return 0;
}

//controller paks contain 32KB * nBanks of SRAM split into 128 pages of 256 bytes each.
//the first 3 + nBanks * 2 pages of bank 0 are for storing system data, and the remaining 123 for game data.
//the remaining banks page 0 is unused and the remaining 127 are for game data.
auto Gamepad::formatControllerPak() -> void {
  ram.fill(0x00);

  //page 0 (system area)
  n6  fieldA = random();
  n19 fieldB = random();
  n27 fieldC = random();
  for(u32 area : array<u8[4]>{1,3,4,6}) {
    ram.write<Byte>(area * 0x20 + 0x01, fieldA);                        //unknown
    ram.write<Word>(area * 0x20 + 0x04, fieldB);                        //serial# hi
    ram.write<Word>(area * 0x20 + 0x08, fieldC);                        //serial# lo
    ram.write<Half>(area * 0x20 + 0x18, 0x0001);                        //device ID
    ram.write<Byte>(area * 0x20 + 0x1a, system.controllerPakBankCount); //banks (0x01 = 32KB), (62 = max banks)
    ram.write<Byte>(area * 0x20 + 0x1b, 0x00);                          //version#
    u16 checksum = 0;
    u16 inverted = 0;
    for(u32 half : range(14)) {
      u16 data = ram.read<Half>(area * 0x20 + half * 2);
      checksum +=  data;
      inverted += ~data;
    }
    ram.write<Half>(area * 0x20 + 0x1c, checksum);
    ram.write<Half>(area * 0x20 + 0x1e, inverted);
  }

  //pages 1 thru nBanks, nBanks+1 thru (nBanks*2) (inode table, inode table copy)
  u8 nBanks = ram.read<Byte>(0x20 + 0x1a);
  u32 inodeTablePage = 1;
  u32 inodeTableCopyPage = 1 + nBanks * 2;
  for(u32 bank : range(0,nBanks)) {
    u32 firstDataPage = bank == 0 ? (3 + nBanks * 2) : 1; //first bank has 3 + bank * 2 system pages, other banks have 127.
    for(u32 page : array<u32[2]>{inodeTablePage + bank, inodeTableCopyPage + bank}) {
      for(u32 slot : range(firstDataPage,128)) {
        ram.write<Byte>(0x100 * page + slot * 2 + 0x01, 0x03);  //0x01 = stop, 0x03 = empty
      }
      ram.write<Byte>(0x100 * page + 0x01, getInodeChecksum(bank));  //checksum
    }
  }

  //page 1 is pak info and serial
  //pages 2-nBanks are for the inode table
  //pages at nBanks+1,2*nBanks are for the inode table backup
  //pages at 2*nBanks+1, 2*nBanks+2 are for note table
  //pages 3 + 2*nBanks are for save data
}

auto Gamepad::serialize(serializer& s) -> void {
  s(ram);
  rumble(false);
}
