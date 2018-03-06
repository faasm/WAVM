#include "Inline/BasicTypes.h"
#include "Inline/Serialization.h"
#include "Platform/Platform.h"
#include "WAST/WAST.h"
#include "WAST/TestScript.h"
#include "WASM/WASM.h"
#include "Runtime/Runtime.h"
#include "Runtime/Linker.h"
#include "Runtime/Intrinsics.h"

#include "CLI.h"

#include <map>
#include <vector>
#include <cstdio>
#include <cstdarg>

using namespace WAST;
using namespace IR;
using namespace Runtime;

DEFINE_INTRINSIC_MODULE(spectest);

struct TestScriptState
{
	bool hasInstantiatedModule;
	GCPointer<ModuleInstance> lastModuleInstance;
	GCPointer<Compartment> compartment;
	GCPointer<Context> context;
	
	GCPointer<ModuleInstance> intrinsicsModule;

	std::map<std::string,GCPointer<ModuleInstance>> moduleInternalNameToInstanceMap;
	std::map<std::string,GCPointer<ModuleInstance>> moduleNameToInstanceMap;
	
	std::vector<WAST::Error> errors;
	
	TestScriptState()
	: hasInstantiatedModule(false)
	, compartment(Runtime::createCompartment())
	, context(Runtime::createContext(compartment))
	{
		intrinsicsModule = Intrinsics::instantiateModule(compartment,spectest);
		moduleNameToInstanceMap["spectest"] = intrinsicsModule;
	}
};

struct TestScriptResolver : Resolver
{
	TestScriptResolver(const TestScriptState& inState): state(inState) {}
	bool resolve(const std::string& moduleName,const std::string& exportName,ObjectType type,Object*& outObject) override
	{
		auto mapIt = state.moduleNameToInstanceMap.find(moduleName);
		if(mapIt != state.moduleNameToInstanceMap.end() && mapIt->second != nullptr)
		{
			outObject = getInstanceExport(mapIt->second,exportName);
			return outObject != nullptr && isA(outObject,type);
		}

		return false;
	}
private:
	const TestScriptState& state;
};

void testErrorf(TestScriptState& state,const TextFileLocus& locus,const char* messageFormat,...)
{
	va_list messageArguments;
	va_start(messageArguments,messageFormat);
	char messageBuffer[1024];
	int numPrintedChars = std::vsnprintf(messageBuffer,sizeof(messageBuffer),messageFormat,messageArguments);
	if(numPrintedChars >= 1023 || numPrintedChars < 0) { Errors::unreachable(); }
	messageBuffer[numPrintedChars] = 0;
	va_end(messageArguments);

	state.errors.push_back({locus,messageBuffer});
}

ModuleInstance* getModuleContextByInternalName(TestScriptState& state,const TextFileLocus& locus,const char* context,const std::string& internalName)
{
	// Look up the module this invoke uses.
	if(!state.hasInstantiatedModule) { testErrorf(state,locus,"no module to use in %s",context); return nullptr; }
	ModuleInstance* moduleInstance = state.lastModuleInstance;
	if(internalName.size())
	{
		auto mapIt = state.moduleInternalNameToInstanceMap.find(internalName);
		if(mapIt == state.moduleInternalNameToInstanceMap.end())
		{
			testErrorf(state,locus,"unknown %s module name: %s",context,internalName.c_str());
			return nullptr;
		}
		moduleInstance = mapIt->second;
	}
	return moduleInstance;
}

bool processAction(TestScriptState& state,Action* action,Result& outResult)
{
	outResult = Result();

	switch(action->type)
	{
	case ActionType::_module:
	{
		auto moduleAction = (ModuleAction*)action;

		// Clear the previous module.
		state.lastModuleInstance = nullptr;
		collectGarbage();

		// Link and instantiate the module.
		TestScriptResolver resolver(state);
		LinkResult linkResult = linkModule(*moduleAction->module,resolver);
		if(linkResult.success)
		{
			state.hasInstantiatedModule = true;
			state.lastModuleInstance = instantiateModule(state.context,*moduleAction->module,std::move(linkResult.resolvedImports));
		}
		else
		{
			// Create an error for each import that couldn't be linked.
			for(auto& missingImport : linkResult.missingImports)
			{
				testErrorf(
					state,
					moduleAction->locus,
					"missing import module=\"%s\" export=\"%s\" type=\"%s\"",
					missingImport.moduleName.c_str(),
					missingImport.exportName.c_str(),
					asString(missingImport.type).c_str()
					);
			}
		}

		// Register the module under its internal name.
		if(moduleAction->internalModuleName.size())
		{
			state.moduleInternalNameToInstanceMap[moduleAction->internalModuleName] = state.lastModuleInstance;
		}

		return true;
	}
	case ActionType::invoke:
	{
		auto invokeAction = (InvokeAction*)action;

		// Look up the module this invoke uses.
		ModuleInstance* moduleInstance = getModuleContextByInternalName(state,invokeAction->locus,"invoke",invokeAction->internalModuleName);

		// A null module instance at this point indicates a module that failed to link or instantiate, so don't produce further errors.
		if(!moduleInstance) { return false; }

		// Find the named export in the module instance.
		auto functionInstance = asFunctionNullable(getInstanceExport(moduleInstance,invokeAction->exportName));
		if(!functionInstance) { testErrorf(state,invokeAction->locus,"couldn't find exported function with name: %s",invokeAction->exportName.c_str()); return false; }

		// Execute the invoke
		outResult = invokeFunction(state.context,functionInstance,invokeAction->arguments);

		return true;
	}
	case ActionType::get:
	{
		auto getAction = (GetAction*)action;

		// Look up the module this get uses.
		ModuleInstance* moduleInstance = getModuleContextByInternalName(state,getAction->locus,"get",getAction->internalModuleName);

		// A null module instance at this point indicates a module that failed to link or instantiate, so just return without further errors.
		if(!moduleInstance) { return false; }

		// Find the named export in the module instance.
		auto globalInstance = asGlobalNullable(getInstanceExport(moduleInstance,getAction->exportName));
		if(!globalInstance) { testErrorf(state,getAction->locus,"couldn't find exported global with name: %s",getAction->exportName.c_str()); return false; }

		// Get the value of the specified global.
		outResult = getGlobalValue(state.context,globalInstance);
			
		return true;
	}
	default:
		Errors::unreachable();
	}
}

// Tests whether a float is a "canonical" NaN, which just means that it's a NaN only the MSB of its significand set.
template<typename Float> bool isCanonicalOrArithmeticNaN(Float value,bool requireCanonical)
{
	Floats::FloatComponents<Float> components;
	components.value = value;
	return components.bits.exponent == Floats::FloatComponents<Float>::maxExponentBits
	&& (!requireCanonical || components.bits.significand == Floats::FloatComponents<Float>::canonicalSignificand);
}

void processCommand(TestScriptState& state,const Command* command)
{
	catchRuntimeExceptions(
		[&]
		{
			switch(command->type)
			{
			case Command::_register:
			{
				auto registerCommand = (RegisterCommand*)command;

				// Look up a module by internal name, and bind the result to the public name.
				ModuleInstance* moduleInstance = getModuleContextByInternalName(state,registerCommand->locus,"register",registerCommand->internalModuleName);
				state.moduleNameToInstanceMap[registerCommand->moduleName] = moduleInstance;
				break;
			}
			case Command::action:
			{
				Result result;
				processAction(state,((ActionCommand*)command)->action.get(),result);
				break;
			}
			case Command::assert_return:
			{
				auto assertCommand = (AssertReturnCommand*)command;
				// Execute the action and do a bitwise comparison of the result to the expected result.
				Result actionResult;
				if(processAction(state,assertCommand->action.get(),actionResult)
				&& !areBitsEqual(actionResult,assertCommand->expectedReturn))
				{
					testErrorf(state,assertCommand->locus,"expected %s but got %s",
						asString(assertCommand->expectedReturn).c_str(),
						asString(actionResult).c_str());
				}
				break;
			}
			case Command::assert_return_canonical_nan: case Command::assert_return_arithmetic_nan:
			{
				auto assertCommand = (AssertReturnNaNCommand*)command;
				// Execute the action and check that the result is a NaN of the expected type.
				Result actionResult;
				if(processAction(state,assertCommand->action.get(),actionResult))
				{
					const bool requireCanonicalNaN = assertCommand->type == Command::assert_return_canonical_nan;
					const bool isError =
							actionResult.type == ResultType::f32 ? !isCanonicalOrArithmeticNaN(actionResult.f32,requireCanonicalNaN)
						:	actionResult.type == ResultType::f64 ? !isCanonicalOrArithmeticNaN(actionResult.f64,requireCanonicalNaN)
						:	true;
					if(isError)
					{
						testErrorf(state,assertCommand->locus,
							requireCanonicalNaN ? "expected canonical float NaN but got %s" : "expected float NaN but got %s",
							asString(actionResult).c_str());
					}
				}
				break;
			}
			case Command::assert_trap:
			{
				auto assertCommand = (AssertTrapCommand*)command;
				Runtime::catchRuntimeExceptions(
					[&]
					{
						Result actionResult;
						if(processAction(state,assertCommand->action.get(),actionResult))
						{
							testErrorf(state,assertCommand->locus,"expected trap but got %s",asString(actionResult).c_str());
						}
					},
					[&](Runtime::Exception&& exception)
					{
						if(exception.type != assertCommand->expectedType)
						{
							testErrorf(state,assertCommand->action->locus,"expected %s trap but got %s trap",
								describeExceptionType(assertCommand->expectedType).c_str(),
								describeExceptionType(exception.type).c_str());
						}
					});
				break;
			}
			case Command::assert_throws:
			{
				auto assertCommand = (AssertThrowsCommand*)command;
				
				// Look up the module containing the expected exception type.
				ModuleInstance* moduleInstance = getModuleContextByInternalName(state,assertCommand->locus,"assert_throws",assertCommand->exceptionTypeInternalModuleName);

				// A null module instance at this point indicates a module that failed to link or instantiate, so don't produce further errors.
				if(!moduleInstance) { break; }

				// Find the named export in the module instance.
				auto expectedExceptionType = asExceptionTypeNullable(getInstanceExport(moduleInstance,assertCommand->exceptionTypeExportName));
				if(!expectedExceptionType)
				{
					testErrorf(state,assertCommand->locus,"couldn't find exported exception type with name: %s",assertCommand->exceptionTypeExportName.c_str());
					break;
				}

				Runtime::catchRuntimeExceptions(
					[&]
					{
						Result actionResult;
						if(processAction(state,assertCommand->action.get(),actionResult))
						{
							testErrorf(state,assertCommand->locus,"expected trap but got %s",asString(actionResult).c_str());
						}
					},
					[&](Runtime::Exception&& exception)
					{
						if(exception.type != expectedExceptionType)
						{
							testErrorf(state,assertCommand->action->locus,"expected %s exception but got %s exception",
								describeExceptionType(expectedExceptionType).c_str(),
								describeExceptionType(exception.type).c_str());
						}
						else
						{
							const TupleType* exceptionParameterTypes = getExceptionTypeParameters(expectedExceptionType);
							assert(exception.arguments.size() == exceptionParameterTypes->elements.size());

							for(Uptr argumentIndex = 0;argumentIndex < exception.arguments.size();++argumentIndex)
							{
								Runtime::Value argumentValue = Runtime::Value(
									exceptionParameterTypes->elements[argumentIndex],
									exception.arguments[argumentIndex]
									);
								if(!areBitsEqual(argumentValue,assertCommand->expectedArguments[argumentIndex]))
								{
									testErrorf(state,assertCommand->locus,"expected %s for exception argument %u but got %s",
										asString(assertCommand->expectedArguments[argumentIndex]).c_str(),
										argumentIndex,
										asString(argumentValue).c_str());
								}
							}
						}
					});
				break;
			}
			case Command::assert_invalid: case Command::assert_malformed:
			{
				auto assertCommand = (AssertInvalidOrMalformedCommand*)command;
				if(!assertCommand->wasInvalidOrMalformed)
				{
					testErrorf(state,assertCommand->locus,"module was %s",
						assertCommand->type == Command::assert_invalid ? "valid" : "well formed");
				}
				break;
			}
			case Command::assert_unlinkable:
			{
				auto assertCommand = (AssertUnlinkableCommand*)command;
				Result result;
				Runtime::catchRuntimeExceptions(
					[&]
					{
						TestScriptResolver resolver(state);
						LinkResult linkResult = linkModule(*assertCommand->moduleAction->module,resolver);
						if(linkResult.success)
						{
							instantiateModule(state.context,*assertCommand->moduleAction->module,std::move(linkResult.resolvedImports));
							testErrorf(state,assertCommand->locus,"module was linkable");
						}
					},
					[&](Runtime::Exception&& exception)
					{
						// If the instantiation throws an exception, the assert_unlinkable succeeds.
					});
				break;
			}
			};
		},
		[&](Runtime::Exception&& exception)
		{
			testErrorf(state,command->locus,"unexpected trap: %s",describeExceptionType(exception.type).c_str());
		});
}

DEFINE_INTRINSIC_FUNCTION0(spectest,spectest_print,print,none) {}
DEFINE_INTRINSIC_FUNCTION1(spectest,spectest_print,print_i32,none,i32,a) { std::cout << asString(a) << " : i32" << std::endl; }
DEFINE_INTRINSIC_FUNCTION1(spectest,spectest_print,print_i64,none,i64,a) { std::cout << asString(a) << " : i64" << std::endl; }
DEFINE_INTRINSIC_FUNCTION1(spectest,spectest_print,print_f32,none,f32,a) { std::cout << asString(a) << " : f32" << std::endl; }
DEFINE_INTRINSIC_FUNCTION1(spectest,spectest_print,print_f64,none,f64,a) { std::cout << asString(a) << " : f64" << std::endl; }
DEFINE_INTRINSIC_FUNCTION2(spectest,spectest_print,print_f64_f64,none,f64,a,f64,b) { std::cout << asString(a) << " : f64" << std::endl << asString(a) << " : f64" << std::endl; }
DEFINE_INTRINSIC_FUNCTION2(spectest,spectest_print,print_i32_f32,none,i32,a,f32,b) { std::cout << asString(a) << " : i32" << std::endl << asString(a) << " : f32" << std::endl; }
DEFINE_INTRINSIC_FUNCTION2(spectest,spectest_print,print_i64_f64,none,i64,a,f64,b) { std::cout << asString(a) << " : i64" << std::endl << asString(a) << " : f64" << std::endl; }

DEFINE_INTRINSIC_GLOBAL(spectest,spectest_globalI32,global_i32,i32,666)
DEFINE_INTRINSIC_GLOBAL(spectest,spectest_globalI64,global_i64,i64,0)
DEFINE_INTRINSIC_GLOBAL(spectest,spectest_globalF32,global_f32,f32,0.0f)
DEFINE_INTRINSIC_GLOBAL(spectest,spectest_globalF64,global_f64,f64,0.0)

DEFINE_INTRINSIC_TABLE(spectest,spectest_table,table,TableType(TableElementType::anyfunc,false,SizeConstraints {10,20}))
DEFINE_INTRINSIC_MEMORY(spectest,spectest_memory,memory,MemoryType(false,SizeConstraints {1,2}))

int commandMain(int argc,char** argv)
{
	if(argc != 2)
	{
		std::cerr <<  "Usage: Test in.wast" << std::endl;
		return EXIT_FAILURE;
	}
	const char* filename = argv[1];
	
	// Always enable debug logging for tests.
	Log::setCategoryEnabled(Log::Category::debug,true);

	Runtime::init();
	
	// Read the file into a string.
	const std::string testScriptString = loadFile(filename);
	if(!testScriptString.size()) { return EXIT_FAILURE; }

	// Process the test script.
	TestScriptState testScriptState;
	std::vector<std::unique_ptr<Command>> testCommands;
	
	// Parse the test script.
	WAST::parseTestCommands(testScriptString.c_str(),testScriptString.size(),testCommands,testScriptState.errors);
	if(!testScriptState.errors.size())
	{
		// Process the test script commands.
		for(auto& command : testCommands)
		{
			processCommand(testScriptState,command.get());
		}
	}
	
	if(testScriptState.errors.size())
	{
		// Print any errors;
		for(auto& error : testScriptState.errors)
		{
			std::cerr << filename << ":" << error.locus.describe() << ": " << error.message.c_str() << std::endl;
			std::cerr << error.locus.sourceLine << std::endl;
			std::cerr << std::setw(error.locus.column(8)) << "^" << std::endl;
		}

		std::cerr << filename << ": testing failed!" << std::endl;
		return EXIT_FAILURE;
	}
	else
	{
		std::cout << filename << ": all tests passed." << std::endl;
		return EXIT_SUCCESS;
	}
}
