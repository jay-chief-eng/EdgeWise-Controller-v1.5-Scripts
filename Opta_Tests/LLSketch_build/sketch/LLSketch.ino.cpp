#include <Arduino.h>
#line 1 "C:\\Users\\swing\\EdgeWise-Controller-v1.5-Scripts\\Opta_Tests\\LLSketch\\LLSketch.ino"
#include <AlPlc_Opta.h>

/* opta_1.3.0
*/

struct PLCSharedVarsInput_t
{
};
PLCSharedVarsInput_t& PLCIn = (PLCSharedVarsInput_t&)m_PLCSharedVarsInputBuf;

struct PLCSharedVarsOutput_t
{
};
PLCSharedVarsOutput_t& PLCOut = (PLCSharedVarsOutput_t&)m_PLCSharedVarsOutputBuf;


AlPlc AxelPLC(264104374, false);

// shared variables can be accessed with PLCIn.varname and PLCOut.varname

// enable direct access to local and expansion I/O variables
// #include <IOPlugin.h>

#line 24 "C:\\Users\\swing\\EdgeWise-Controller-v1.5-Scripts\\Opta_Tests\\LLSketch\\LLSketch.ino"
void setup();
#line 33 "C:\\Users\\swing\\EdgeWise-Controller-v1.5-Scripts\\Opta_Tests\\LLSketch\\LLSketch.ino"
void loop();
#line 24 "C:\\Users\\swing\\EdgeWise-Controller-v1.5-Scripts\\Opta_Tests\\LLSketch\\LLSketch.ino"
void setup()
{


	AxelPLC.InitFileSystem();

	AxelPLC.Run();
}

void loop()
{
	delay(1);
	

}

