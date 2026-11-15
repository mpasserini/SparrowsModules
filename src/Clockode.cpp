#include "plugin.hpp"


struct Clockode : Module {
	enum ParamIds {
		TEMPO_PARAM,
		RUN_PARAM,
		TRIG_PARAM,
		ENUMS(CV_PARAMS, 16),
		ENUMS(GATE_PARAMS, 16),
		TEMPO_CV_PARAM,
		STEPS_CV_PARAM,
		CLOCK_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TEMPO_INPUT,
		CLOCK_INPUT,
		STEPS_INPUT,
		RUN_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		TRIG_OUTPUT,
		ENUMS(STEP_OUTPUTS, 16),
		RUN_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CLOCK_LIGHT,
		RUN_LIGHT,
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

	dsp::PulseGenerator runPulse;
	dsp::PulseGenerator clockPulse;
	dsp::PulseGenerator stepPulses[16];

	/** Phase of internal LFO */
	float phase = 0.f;
	int index = 0;
	bool gates[16] = {};

	Clockode() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		configParam(TEMPO_PARAM, -2.f, 4.f, 1.f, "Tempo", " bpm", 2.f, 60.f);
		getParamQuantity(TEMPO_PARAM)->randomizeEnabled = false;
		configParam(TEMPO_CV_PARAM, 0.f, 1.f, 1.f, "Tempo CV", "%", 0, 100);
		getParamQuantity(TEMPO_CV_PARAM)->randomizeEnabled = false;
		configButton(CLOCK_PARAM, "Clock");
		configButton(RUN_PARAM, "Run");
		configParam(TRIG_PARAM, 1.f, 16.f, 16.f, "Steps");
		getParamQuantity(TRIG_PARAM)->randomizeEnabled = false;
		configParam(STEPS_CV_PARAM, 0.f, 1.f, 1.f, "Steps CV", "%", 0, 100);
		getParamQuantity(STEPS_CV_PARAM)->randomizeEnabled = false;
		paramQuantities[TRIG_PARAM]->snapEnabled = true;
		
		for (int i = 0; i < 16; i++) {
			configParam(CV_PARAMS + i, -10.f, 10.f, 0.f, string::f("CV %d step %d", i + 1), " V");
		}
		
		for (int i = 0; i < 16; i++) {
			configButton(GATE_PARAMS + i, string::f("Step %d trigger", i + 1));
		}

		configInput(TEMPO_INPUT, "Tempo");
		configInput(CLOCK_INPUT, "Clock");
		configInput(RUN_INPUT, "Run");
		configInput(STEPS_INPUT, "Steps");

		for (int i = 0; i < 16; i++) {
			configOutput(STEP_OUTPUTS + i, string::f("Step %d", i + 1));
		}
		configOutput(TRIG_OUTPUT, "Trigger");
		configOutput(RUN_OUTPUT, "Run");

	}


	void onRandomize() override {
		for (int i = 0; i < 16; i++) {
			gates[i] = random::get<bool>();
		}
	}

	void rotateStates(int delta) {
		// Rotate CV params
		
		float cvs[16];
		for (int i = 0; i < 16; i++) {
			cvs[i] = params[CV_PARAMS + i].getValue();
		}
		for (int i = 0; i < 16; i++) {
			int index = eucMod(i + delta, 16);
			params[CV_PARAMS + index].setValue(cvs[i]);
		}
		
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

	float stepOffset(int step) {
		// Timing offset as a fraction of the clock period: -0.5 (early) .. +0.5 (late)
		return params[CV_PARAMS + step].getValue() / 10.f * 0.5f;
	}

	float extTimer = 0.f;
	float extPeriod = 0.f;
	bool extClockSeen = false;
	bool extClockConnected = false;
	float pendingTime = 0.f;
	int pendingStep = -1;
	bool earlyFired = false;

	void fireStep(int i) {
		index = i;
		clockPulse.trigger(1e-3f);
		stepPulses[i].trigger(1e-3f);
	}

	void process(const ProcessArgs& args) override {
		// Toggle run
		bool runButtonTriggered = runButtonTrigger.process(params[RUN_PARAM].getValue());
		bool runTriggered = runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f);
		if (runButtonTriggered || runTriggered) {
			running ^= true;
			runPulse.trigger(1e-3f);
		}
		bool runGate = runPulse.process(args.sampleTime);

		// Get number of steps
		float steps = params[TRIG_PARAM].getValue() + inputs[STEPS_INPUT].getVoltage() * params[STEPS_CV_PARAM].getValue();
		int numSteps = (int) clamp(std::round(steps), 1.f, 16.f);

		// Clock
		bool clock = false;
		bool clockGate = false;
		float period = 0.f;
		bool connected = inputs[CLOCK_INPUT].isConnected();
		if (connected != extClockConnected) {
			extClockConnected = connected;
			extTimer = 0.f;
			extPeriod = 0.f;
			extClockSeen = false;
			earlyFired = false;
		}
		if (running) {
			bool clockButton = clockButtonTrigger.process(params[CLOCK_PARAM].getValue());
			if (connected) {
				// Predict the next external edge from the last measured period.
				extTimer += args.sampleTime;
				bool edge = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f);
				if (edge) {
					if (extClockSeen && extTimer > 1e-4f)
						extPeriod = extTimer;
					extTimer = 0.f;
					extClockSeen = true;
				}
				clock = edge || clockButton;
				period = extPeriod;
				clockGate = clockTrigger.isHigh() || clockButtonTrigger.isHigh();

				if (!clock && !earlyFired && pendingStep < 0 && extPeriod > 0.f) {
					int next = (index + 1 >= numSteps) ? 0 : index + 1;
					float off = stepOffset(next);
					if (off < 0.f && extTimer >= extPeriod * (1.f + off)) {
						fireStep(next);
						earlyFired = true;
					}
				}
			}
			else {
				// Internal clock
				float clockPitch = params[TEMPO_PARAM].getValue() + inputs[TEMPO_INPUT].getVoltage() * params[TEMPO_CV_PARAM].getValue();
				float clockFreq = dsp::exp2_taylor5(clockPitch);
				period = 1.f / clockFreq;
				phase += clockFreq * args.sampleTime;
				if (phase >= 1.f) {
					clock = true;
					phase -= std::trunc(phase);
				}
				if (clockButton) {
					clock = true;
					phase = 0.f;
				}
				clockGate = (phase < 0.5f);

				// Early steps (internal clock only): fire before the clock edge
				if (!clock && !earlyFired && pendingStep < 0) {
					int next = (index + 1 >= numSteps) ? 0 : index + 1;
					float off = stepOffset(next);
					if (off < 0.f && phase >= 1.f + off) {
						fireStep(next);
						earlyFired = true;
					}
				}
			}
		}

		// Advance step when clocked
		if (clock) {
			if (earlyFired) {
				earlyFired = false;
			}
			else {
				int next = (index + 1 >= numSteps) ? 0 : index + 1;
				float off = stepOffset(next);
				if (off > 0.f && period > 0.f) {
					// Late: schedule after a fraction of the clock period
					pendingStep = next;
					pendingTime = off * period;
				}
				else {
					fireStep(next);
				}
			}
		}
		if (pendingStep >= 0) {
			pendingTime -= args.sampleTime;
			if (pendingTime <= 0.f) {
				fireStep(pendingStep);
				pendingStep = -1;
			}
		}

		// Unless we're passing the clock gate, generate a pulse
		if (!clockPassthrough) {
			clockGate = clockPulse.process(args.sampleTime);
		}

		// Gate buttons
		for (int i = 0; i < 16; i++) {
			if (gateTriggers[i].process(params[GATE_PARAMS + i].getValue())) {
				gates[i] ^= true;
			}
			lights[GATE_LIGHTS + i].setBrightness(gates[i]);
		}

		// Step outputs
		for (int i = 0; i < 16; i++) {
			outputs[STEP_OUTPUTS + i].setVoltage(stepPulses[i].process(args.sampleTime) ? 10.f : 0.f);
			lights[STEP_LIGHTS + 2 * i + 0].setSmoothBrightness(index == i, args.sampleTime);
			lights[STEP_LIGHTS + 2 * i + 1].setBrightness(i >= numSteps);
		}

		// Outputs
		outputs[TRIG_OUTPUT].setVoltage((clockGate && gates[index]) ? 10.f : 0.f);

		outputs[RUN_OUTPUT].setVoltage(runGate ? 10.f : 0.f);

		lights[CLOCK_LIGHT].setSmoothBrightness(clockGate, args.sampleTime);
		lights[RUN_LIGHT].setBrightness(running);
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


struct ClockodeWidget : ModuleWidget {
	ClockodeWidget(Clockode* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Clockode.svg"), asset::plugin(pluginInstance, "res/Clockode-dark.svg")));

		//addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		//addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		//addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		//addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));



		for (int i = 0; i < 16; i++) {
			addChild(createLightCentered<TinyLight<YellowRedLight<>>>(mm2px(Vec(20, 5 + i * 8)), module, Clockode::STEP_LIGHTS + 2 * i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(25, 5 + i * 8)), module, Clockode::CV_PARAMS + i));
			// gate on/off buttons
			addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(35, 5 + i * 8)), module, Clockode::GATE_PARAMS + i, Clockode::GATE_LIGHTS + i));
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(45, 5 + i * 8)), module, Clockode::STEP_OUTPUTS + i));
		}


		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 0 * 8)), module, Clockode::TEMPO_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 1 * 8)), module, Clockode::TRIG_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 2 * 8)), module, Clockode::TEMPO_CV_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 3 * 8)), module, Clockode::STEPS_CV_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(5.0, 15 + 4 * 8)), module, Clockode::CLOCK_PARAM, Clockode::CLOCK_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(5.0, 15 + 5 * 8)), module, Clockode::RUN_PARAM, Clockode::RUN_LIGHT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 6 * 8)), module, Clockode::TEMPO_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 7 * 8)), module, Clockode::STEPS_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 8 * 8)), module, Clockode::CLOCK_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 9 * 8)), module, Clockode::RUN_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 10 * 8)), module, Clockode::TRIG_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 11 * 8)), module, Clockode::RUN_OUTPUT));

	}

	void appendContextMenu(Menu* menu) override {
		Clockode* module = dynamic_cast<Clockode*>(this->module);
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



Model* modelClockode = createModel<Clockode, ClockodeWidget>("Clockode");
