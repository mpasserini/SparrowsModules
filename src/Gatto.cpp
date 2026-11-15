#include "plugin.hpp"


struct Gatto : Module {
	enum ParamIds {
		TRIG_PARAM,
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
		GATE_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CLOCK_LIGHT,
		ENUMS(STEP_LIGHTS, 16 * 2),
		NUM_LIGHTS
	};

	bool running = true;

	dsp::BooleanTrigger clockButtonTrigger;

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger runTrigger;
	dsp::SchmittTrigger resetTrigger;

	dsp::PulseGenerator clockPulse;

	int index = -1;
	float clockElapsed = 0.f;
	float clockPeriod = 0.f;
	bool hasClockReference = false;
	bool hasClockPeriod = false;

	Gatto() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		
		configButton(CLOCK_PARAM, "Clock");
		configParam(TRIG_PARAM, 1.f, 16.f, 16.f, "Steps");
		getParamQuantity(TRIG_PARAM)->randomizeEnabled = false;
		paramQuantities[TRIG_PARAM]->snapEnabled = true;
		
		for (int i = 0; i < 16; i++) {
			configParam(GATE_PARAMS + i, 0.f, 100.f, 50.f, string::f("Gate length step %d", i + 1), "%");
		}
		
		configInput(CLOCK_INPUT, "Clock");
		configInput(RUN_INPUT, "Run");
		configInput(STEPS_INPUT, "Steps");
		configInput(RESET_INPUT, "Reset");

		
		configOutput(GATE_OUTPUT, "Gate");

	}

	void rotateStates(int delta) {
		// Rotate CV params
		
		float cvs[16];
		for (int i = 0; i < 16; i++) {
			cvs[i] = params[GATE_PARAMS + i].getValue();
		}
		for (int i = 0; i < 16; i++) {
			int index = eucMod(i + delta, 16);
			params[GATE_PARAMS + index].setValue(cvs[i]);
		}
		
	}

	void process(const ProcessArgs& args) override {
		// Toggle run
		bool runTriggered = runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f);
		if (runTriggered)
			running ^= true;

		// Clock
		bool clock = false;
		if (running) {
			bool clockButton = clockButtonTrigger.process(params[CLOCK_PARAM].getValue());
			if (inputs[CLOCK_INPUT].isConnected()) {
				// External clock
				bool clockTriggered = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
				if (clockTriggered || clockButton) {
					clock = true;
				}
			}
			else if (clockButton) {
				clock = true;
			}
		}

		// Get number of steps
		float steps = params[TRIG_PARAM].getValue() + inputs[STEPS_INPUT].getVoltage() ;
		int numSteps = (int) clamp(std::round(steps), 1.f, 16.f);

		// Advance step when clocked
		bool gateWasHigh = false;
		bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		clockElapsed += args.sampleTime;
		if (clock && !reset) {
			if (hasClockReference) {
				clockPeriod = clockElapsed;
				hasClockPeriod = true;
			}
			hasClockReference = true;
			clockElapsed = 0.f;
		}
		if (reset) {
			index = 0;
			clock = true;
			// Treat reset as the start of a new clock interval. Otherwise the
			// next clock can include time from before reset and overextend step 2.
			clockElapsed = 0.f;
			hasClockReference = true;
			clockPulse.reset();
		}
		if (clock) {
			if (!reset) {
				index++;
				if (index >= numSteps)
					index = 0;
			}
			// Gate length is a percentage of the previous clock interval.
			float len = params[GATE_PARAMS + index].getValue() / 100.f;
			// Replace any remaining time from the previous step.
			gateWasHigh = clockPulse.remaining > 0.f;
			clockPulse.reset();
			if (hasClockPeriod && len > 0.f)
				clockPulse.trigger(clockPeriod * len);
		}
		bool clockGate = clockPulse.process(args.sampleTime);
		// Insert a one-sample gap so consecutive gates don't merge (e.g. a 100% step followed by a 50% step)
		if (gateWasHigh)
			clockGate = false;


		// Step outputs
		for (int i = 0; i < 16; i++) {
			lights[STEP_LIGHTS + 2 * i + 0].setSmoothBrightness(index == i, args.sampleTime);
			lights[STEP_LIGHTS + 2 * i + 1].setBrightness(i >= numSteps);
		}

		// Outputs
		outputs[GATE_OUTPUT].setVoltage(clockGate ? 10.f : 0.f);

		lights[CLOCK_LIGHT].setSmoothBrightness(clockGate, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		// running
		json_object_set_new(rootJ, "running", json_boolean(running));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		// running
		json_t* runningJ = json_object_get(rootJ, "running");
		if (runningJ)
			running = json_is_true(runningJ);
	}
};



struct GattoWidget : ModuleWidget {
	GattoWidget(Gatto* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Gatto.svg"), asset::plugin(pluginInstance, "res/Gatto-dark.svg")));

		for (int i = 0; i < 16; i++) {
			addChild(createLightCentered<TinyLight<YellowRedLight<>>>(mm2px(Vec(20, 5 + i * 8)), module, Gatto::STEP_LIGHTS + 2 * i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(25, 5 + i * 8)), module, Gatto::GATE_PARAMS + i));

		}

		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 1 * 8)), module, Gatto::TRIG_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(5.0, 15 + 4 * 8)), module, Gatto::CLOCK_PARAM, Gatto::CLOCK_LIGHT));
	
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 7 * 8)), module, Gatto::STEPS_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 8 * 8)), module, Gatto::CLOCK_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 9 * 8)), module, Gatto::RUN_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 6 * 8)), module, Gatto::RESET_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 10 * 8)), module, Gatto::GATE_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Gatto* module = dynamic_cast<Gatto*>(this->module);
		assert(module);
		menu->addChild(createMenuItem("Rotate left", "",
			[=]() {module->rotateStates(-1);}
		));

		menu->addChild(createMenuItem("Rotate right", "",
			[=]() {module->rotateStates(1);}
		));
	}
};



Model* modelGatto = createModel<Gatto, GattoWidget>("Gatto");
