#include "plugin.hpp"


struct Triggo : Module {
	enum ParamIds {
		RUN_PARAM,
		NSTEPS_PARAM,
		ENUMS(GATE_PARAMS, 16),
		CLOCK_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		CLOCK_INPUT,
		STEPS_INPUT,
		RUN_INPUT,
		RESET_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		TRIG_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CLOCK_LIGHT,
		ENUMS(GATE_LIGHTS, 16),
		ENUMS(STEP_LIGHTS, 16 * 2),
		NUM_LIGHTS
	};

	bool running = true;
	bool clockPassthrough = false;

	dsp::BooleanTrigger clockButtonTrigger;
	dsp::BooleanTrigger runButtonTrigger;
	dsp::BooleanTrigger gateTriggers[16];

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger runTrigger;
	dsp::SchmittTrigger resetTrigger;

	dsp::PulseGenerator clockPulse;

	int index = 0;
	bool gates[16] = {};

	Triggo() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		
		configButton(CLOCK_PARAM, "Clock");
		configButton(RUN_PARAM, "Run");
		configParam(NSTEPS_PARAM, 1.f, 16.f, 16.f, "Steps");
		getParamQuantity(NSTEPS_PARAM)->randomizeEnabled = false;
		
		paramQuantities[NSTEPS_PARAM]->snapEnabled = true;
		
		
		
		for (int i = 0; i < 16; i++) {
			configButton(GATE_PARAMS + i, string::f("Step %d trigger", i + 1));
		}

		configInput(CLOCK_INPUT, "Clock");
		configInput(RUN_INPUT, "Run");
		configInput(STEPS_INPUT, "Steps");
		configInput(RESET_INPUT, "Reset");

		configOutput(TRIG_OUTPUT, "Accent trigger");

	}


	void onRandomize() override {
		for (int i = 0; i < 16; i++) {
			gates[i] = random::get<bool>();
		}
	}

	void rotateStates(int delta) {
		// Rotate gates
		bool gates[16];
		for (int i = 0; i < 16; i++) {
			gates[i] = this->gates[i];
		}
		for (int i = 0; i < 16; i++) {
			int index = eucMod(i + delta, 16);
			this->gates[index] = gates[i];
		}
	}

	void process(const ProcessArgs& args) override {
		// Toggle run
		bool runButtonTriggered = runButtonTrigger.process(params[RUN_PARAM].getValue());
		bool runTriggered = runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f);
		if (runButtonTriggered || runTriggered) {
			running ^= true;
		}

		// Get number of steps
		float steps = params[NSTEPS_PARAM].getValue() + inputs[STEPS_INPUT].getVoltage();
		int numSteps = (int) clamp(std::round(steps), 1.f, 16.f);

		bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		bool clockButton = clockButtonTrigger.process(params[CLOCK_PARAM].getValue());
		bool externalClock = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
		bool clock = running && (clockButton || externalClock);
		if (reset)
			index = 0;
		if (clock) {
			if (!reset) {
				index++;
				if (index >= numSteps)
					index = 0;
			}
			clockPulse.trigger(1e-3f);
		}

		bool pulse = clockPulse.process(args.sampleTime);
		bool clockGate = running && (clockPassthrough
			? (clockTrigger.isHigh() || clockButtonTrigger.isHigh())
			: pulse);

		// Gate buttons
		for (int i = 0; i < 16; i++) {
			if (gateTriggers[i].process(params[GATE_PARAMS + i].getValue())) {
				gates[i] ^= true;
			}
			lights[GATE_LIGHTS + i].setBrightness(gates[i]);
		}

		// Step outputs
		for (int i = 0; i < 16; i++) {
			lights[STEP_LIGHTS + 2 * i + 0].setSmoothBrightness(index == i, args.sampleTime);
			lights[STEP_LIGHTS + 2 * i + 1].setBrightness(i >= numSteps);
		}

		// Outputs
		outputs[TRIG_OUTPUT].setVoltage((clockGate && gates[index]) ? 10.f : 0.f);

		lights[CLOCK_LIGHT].setSmoothBrightness(clockGate, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		// running
		json_object_set_new(rootJ, "running", json_boolean(running));

		// gates
		json_t* gatesJ = json_array();
		for (int i = 0; i < 16; i++) {
			json_array_insert_new(gatesJ, i, json_integer((int) gates[i]));
		}
		json_object_set_new(rootJ, "gates", gatesJ);

		// clockPassthrough
		json_object_set_new(rootJ, "clockPassthrough", json_boolean(clockPassthrough));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		// running
		json_t* runningJ = json_object_get(rootJ, "running");
		if (runningJ)
			running = json_is_true(runningJ);

		// gates
		json_t* gatesJ = json_object_get(rootJ, "gates");
		if (gatesJ) {
			for (int i = 0; i < 16; i++) {
				json_t* gateJ = json_array_get(gatesJ, i);
				if (gateJ)
					gates[i] = !!json_integer_value(gateJ);
			}
		}

		// clockPassthrough
		json_t* clockPassthroughJ = json_object_get(rootJ, "clockPassthrough");
		if (clockPassthroughJ)
			clockPassthrough = json_is_true(clockPassthroughJ);
		else
			clockPassthrough = true;
	}
};


struct TriggoWidget : ModuleWidget {
	TriggoWidget(Triggo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Triggo.svg"), asset::plugin(pluginInstance, "res/Triggo-dark.svg")));

		for (int i = 0; i < 16; i++) {
			addChild(createLightCentered<TinyLight<YellowRedLight<>>>(mm2px(Vec(20, 5 + i * 8)), module, Triggo::STEP_LIGHTS + 2 * i));

			// gate on/off buttons
			addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(25, 5 + i * 8)), module, Triggo::GATE_PARAMS + i, Triggo::GATE_LIGHTS + i));

		}

		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 1 * 8)), module, Triggo::NSTEPS_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(5.0, 15 + 4 * 8)), module, Triggo::CLOCK_PARAM, Triggo::CLOCK_LIGHT));



		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 6 * 8)), module, Triggo::RESET_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 7 * 8)), module, Triggo::STEPS_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 8 * 8)), module, Triggo::CLOCK_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 9 * 8)), module, Triggo::RUN_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 10 * 8)), module, Triggo::TRIG_OUTPUT));
		

	}

	void appendContextMenu(Menu* menu) override {
		Triggo* module = dynamic_cast<Triggo*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);

		menu->addChild(createBoolPtrMenuItem("Clock passthrough", "", &module->clockPassthrough));

		menu->addChild(new MenuSeparator);

		menu->addChild(createMenuItem("Rotate left", "",
			[=]() {module->rotateStates(-1);}
		));

		menu->addChild(createMenuItem("Rotate right", "",
			[=]() {module->rotateStates(1);}
		));
	}
};



Model* modelTriggo = createModel<Triggo, TriggoWidget>("Triggo");
