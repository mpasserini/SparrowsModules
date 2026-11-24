#include "plugin.hpp"


struct Civciv : Module {
	enum ParamIds {
		NSTEPS_PARAM,
		QUANT_PARAM,
		SLIDE_TIME_PARAM,
		ENUMS(CV_PARAMS, 16),
		CLOCK_PARAM,
		ENUMS(OCT_PARAMS, 16),
		ENUMS(SLIDE_PARAMS, 16),
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
		CV_OUTPUT,
		EOC_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CLOCK_LIGHT,
		ENUMS(SLIDE_LIGHTS, 16),
		ENUMS(STEP_LIGHTS, 16 * 2),
		NUM_LIGHTS
	};

	bool running = true;

	dsp::BooleanTrigger clockButtonTrigger;
	dsp::BooleanTrigger slideTriggers[16];

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger runTrigger;
	dsp::SchmittTrigger resetTrigger;

	dsp::PulseGenerator clockPulse;
	dsp::PulseGenerator eocPulse;

	int index = 0;
	bool slides[16] = {};
	float clockElapsed = 0.f;
	float clockPeriod = 0.f;
	bool haveClock = false;

	Civciv() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		
		configButton(CLOCK_PARAM, "Clock");
		configParam(NSTEPS_PARAM, 1.f, 16.f, 16.f, "Steps");
		getParamQuantity(NSTEPS_PARAM)->randomizeEnabled = false;
		configParam(QUANT_PARAM, 0.f, 60.f, 12.f, "Quantization");
		configParam(SLIDE_TIME_PARAM, 0.f, 2.f, 0.5f, "Slide time", " s");
		getParamQuantity(QUANT_PARAM)->snapEnabled = true;
		
		paramQuantities[NSTEPS_PARAM]->snapEnabled = true;
		
		for (int i = 0; i < 16; i++) {
			configParam(CV_PARAMS + i, 0.f, 1.f, 0.5f, string::f("Fine pitch step %d", i + 1), " V");
			configSwitch(OCT_PARAMS + i, 0.f, 2.f, 0.f, string::f("Octave step %d", i + 1), {"+0 V", "+1 V", "+2 V"});
			configButton(SLIDE_PARAMS + i, string::f("Slide step %d", i + 1));
		}
		
		configInput(CLOCK_INPUT, "Clock");
		configInput(RUN_INPUT, "Run");
		configInput(STEPS_INPUT, "Steps");
		configInput(RESET_INPUT, "Reset");

		
		configOutput(CV_OUTPUT, "CV");
		configOutput(EOC_OUTPUT, "End of cycle");

	}


	void onRandomize() override {
		for (int i = 0; i < 16; i++) {
			slides[i] = random::get<bool>();
		}
	}

	void rotateStates(int delta) {
		float cvs[16];
		float octaves[16];
		bool slideStates[16];
		for (int i = 0; i < 16; i++) {
			cvs[i] = params[CV_PARAMS + i].getValue();
			octaves[i] = params[OCT_PARAMS + i].getValue();
			slideStates[i] = slides[i];
		}
		for (int i = 0; i < 16; i++) {
			int index = eucMod(i + delta, 16);
			params[CV_PARAMS + index].setValue(cvs[i]);
			params[OCT_PARAMS + index].setValue(octaves[i]);
			slides[index] = slideStates[i];
		}
	}

	void process(const ProcessArgs& args) override {
		// Toggle run
		bool runTriggered = runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 2.f);
		if (runTriggered) {
			running ^= true;
		}

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
		float steps = params[NSTEPS_PARAM].getValue() + inputs[STEPS_INPUT].getVoltage() ;
		int numSteps = (int) clamp(std::round(steps), 1.f, 16.f);

		// Advance step when clocked
		bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f);
		if (reset) {
			index = 0;
			clock = true;
			clockPulse.reset();
		}
		if (clock) {
			if (!reset && haveClock && clockElapsed > 0.f)
				clockPeriod = clockElapsed;
			clockElapsed = 0.f;
			haveClock = true;
			if (!reset) {
				index++;
				if (index >= numSteps) {
					index = 0;
					eocPulse.trigger(1e-3f);
				}
			}
			clockPulse.trigger(1e-3f);
		}
		bool clockGate = clockPulse.process(args.sampleTime);


		// Slide buttons
		for (int i = 0; i < 16; i++) {
			if (slideTriggers[i].process(params[SLIDE_PARAMS + i].getValue())) {
				slides[i] ^= true;
			}
			lights[SLIDE_LIGHTS + i].setBrightness(slides[i]);
		}

		// Step outputs
		for (int i = 0; i < 16; i++) {
			lights[STEP_LIGHTS + 2 * i + 0].setSmoothBrightness(index == i, args.sampleTime);
			lights[STEP_LIGHTS + 2 * i + 1].setBrightness(i >= numSteps);
		}

		// Outputs
		int quantization = (int) std::round(params[QUANT_PARAM].getValue());
		int oct = (int) std::round(params[OCT_PARAMS + index].getValue());
		oct = clamp(oct, 0, 2);
		float fine = params[CV_PARAMS + index].getValue();
		if (quantization == 1)
			fine = 0.f;
		else if (quantization > 1)
			fine = std::round(fine * (quantization - 1)) / (quantization - 1);
		float cv = fine + (float) oct;
		float slideTime = params[SLIDE_TIME_PARAM].getValue();
		float slideDuration = slideTime > 0.f ? clamp(slideTime, 0.01f, 2.f) : clockPeriod;
		if (slides[index] && slideDuration > 0.f) {
			int next = (index + 1) % numSteps;
			int nextOct = clamp((int) std::round(params[OCT_PARAMS + next].getValue()), 0, 2);
			float nextFine = params[CV_PARAMS + next].getValue();
			if (quantization == 1)
				nextFine = 0.f;
			else if (quantization > 1)
				nextFine = std::round(nextFine * (quantization - 1)) / (quantization - 1);
			float nextCv = nextFine + (float) nextOct;
			float progress = clamp(clockElapsed / slideDuration, 0.f, 1.f);
			cv += (nextCv - cv) * progress;
		}
		outputs[CV_OUTPUT].setVoltage(cv);
		outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
		if (running && haveClock)
			clockElapsed += args.sampleTime;

		lights[CLOCK_LIGHT].setSmoothBrightness(clockGate, args.sampleTime);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		// running
		json_object_set_new(rootJ, "running", json_boolean(running));

		// Keep the legacy key for saved patches.
		json_t* gatesJ = json_array();
		for (int i = 0; i < 16; i++) {
			json_array_insert_new(gatesJ, i, json_integer((int) slides[i]));
		}
		json_object_set_new(rootJ, "gates", gatesJ);

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		// running
		json_t* runningJ = json_object_get(rootJ, "running");
		if (runningJ)
			running = json_is_true(runningJ);

		// Slide states (stored under the legacy key).
		json_t* gatesJ = json_object_get(rootJ, "gates");
		if (gatesJ) {
			for (int i = 0; i < 16; i++) {
				json_t* gateJ = json_array_get(gatesJ, i);
				if (gateJ)
					slides[i] = !!json_integer_value(gateJ);
			}
		}
	}
};



struct CivcivWidget : ModuleWidget {
	CivcivWidget(Civciv* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Civciv.svg"), asset::plugin(pluginInstance, "res/Civciv-dark.svg")));

		for (int i = 0; i < 16; i++) {
			addChild(createLightCentered<TinyLight<YellowRedLight<>>>(mm2px(Vec(20, 5 + i * 8)), module, Civciv::STEP_LIGHTS + 2 * i));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(25, 5 + i * 8)), module, Civciv::CV_PARAMS + i));
			addParam(createParamCentered<CKSSThreeHorizontal>(mm2px(Vec(35, 5 + i * 8)), module, Civciv::OCT_PARAMS + i));

			addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(45, 5 + i * 8)), module, Civciv::SLIDE_PARAMS + i, Civciv::SLIDE_LIGHTS + i));
		}

		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 1 * 8)), module, Civciv::NSTEPS_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 2 * 8)), module, Civciv::QUANT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(5.0, 15 + 3 * 8)), module, Civciv::SLIDE_TIME_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(5.0, 15 + 4 * 8)), module, Civciv::CLOCK_PARAM, Civciv::CLOCK_LIGHT));
	
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 7 * 8)), module, Civciv::STEPS_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 8 * 8)), module, Civciv::CLOCK_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 9 * 8)), module, Civciv::RUN_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 6 * 8)), module, Civciv::RESET_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 10 * 8)), module, Civciv::CV_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(5.0, 15 + 11 * 8)), module, Civciv::EOC_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Civciv* module = dynamic_cast<Civciv*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);

		menu->addChild(createMenuItem("Rotate left", "",
			[=]() {module->rotateStates(-1);}
		));

		menu->addChild(createMenuItem("Rotate right", "",
			[=]() {module->rotateStates(1);}
		));
	}
};


Model* modelCivciv = createModel<Civciv, CivcivWidget>("Civciv");

