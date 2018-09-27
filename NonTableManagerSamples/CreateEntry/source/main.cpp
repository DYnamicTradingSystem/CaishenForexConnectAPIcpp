#include "stdafx.h"
#include "ResponseListener.h"
#include "SessionStatusListener.h"
#include "LoginParams.h"
#include "SampleParams.h"
#include "CommonSources.h"

void printHelp(std::string &);
bool checkObligatoryParams(LoginParams *, SampleParams *);
void printSampleParams(std::string &, LoginParams *, SampleParams *);
IO2GRequest *createEntryOrderRequest(IO2GSession *, const char *, const char *, int, double, const char *, const char *, const char *);
std::string getEntryOrderType(double, double, double, const char *, double, int, int);

int roundPrice(double x)
{
	return (int)(floor(x + 0.5));
}

int main(int argc, char *argv[])
{
	std::string procName = "CreateEntry";
	if (argc == 1)
	{
		printHelp(procName);
		return -1;
	}

	bool bWasError = false;

	LoginParams *loginParams = new LoginParams(argc, argv);
	SampleParams *sampleParams = new SampleParams(argc, argv);

	printSampleParams(procName, loginParams, sampleParams);
	if (!checkObligatoryParams(loginParams, sampleParams))
	{
		delete loginParams;
		delete sampleParams;
		return -1;
	}

	IO2GSession *session = CO2GTransport::createSession();

	SessionStatusListener *sessionListener = new SessionStatusListener(session, true,
		loginParams->getSessionID(),
		loginParams->getPin());
	session->subscribeSessionStatus(sessionListener);

	bool bConnected = login(session, sessionListener, loginParams);

	if (bConnected)
	{
		bool bIsAccountEmpty = !sampleParams->getAccount() || strlen(sampleParams->getAccount()) == 0;
		O2G2Ptr<IO2GAccountRow> account = getAccount(session, sampleParams->getAccount());
		ResponseListener *responseListener = new ResponseListener(session);
		session->subscribeResponse(responseListener);
		if (account)
		{
			if (bIsAccountEmpty)
			{
				sampleParams->setAccount(account->getAccountID());

				//json
				std::cout <<  "{\"Account\":{\"Id\":\"" << sampleParams->getAccount() << "\"}}"  << std::endl;
			}
			O2G2Ptr<IO2GOfferRow> offer = getOffer(session, sampleParams->getInstrument());
			if (offer)
			{
				O2G2Ptr<IO2GLoginRules> loginRules = session->getLoginRules();
				if (loginRules)
				{
					O2G2Ptr<IO2GTradingSettingsProvider> tradingSettingsProvider = loginRules->getTradingSettingsProvider();
					int iBaseUnitSize = tradingSettingsProvider->getBaseUnitSize(sampleParams->getInstrument(), account);
					int iAmount = iBaseUnitSize * sampleParams->getLots();
					int iCondDistEntryLimit = tradingSettingsProvider->getCondDistEntryLimit(sampleParams->getInstrument());
					int iCondDistEntryStop = tradingSettingsProvider->getCondDistEntryStop(sampleParams->getInstrument());

					std::string sOrderType = getEntryOrderType(offer->getBid(), offer->getAsk(), sampleParams->getRate(),
						sampleParams->getBuySell(), offer->getPointSize(), iCondDistEntryLimit, iCondDistEntryStop);

					O2G2Ptr<IO2GRequest> request = createEntryOrderRequest(session, offer->getOfferID(), account->getAccountID(), iAmount,
						sampleParams->getRate(), sampleParams->getBuySell(), sOrderType.c_str(), sampleParams->getExpDate());
					if (request)
					{
						responseListener->setRequestID(request->getRequestID());
						session->sendRequest(request);
						if (responseListener->waitEvents())
						{

							std::cout << "{\"status\":{\"message\":\done!\",\"state\":\"done\"}" << std::endl;

						}
						else
						{
							std::cout << "{\"status\":{\"message\":\"Response waiting timeout expired\",\"state\":\"failed\"}" << std::endl;
							bWasError = true;
						}
					}
					else
					{
						std::cout << "{\"status\":{\"message\":\"Can not create request\",\"state\":\"failed\"}" << std::endl;

						bWasError = true;
					}
				}
				else
				{
					std::cout << "{\"status\":{\"message\":\"Cannot get login rules\",\"state\":\"failed\"}" << std::endl;

					bWasError = true;
				}
			}
			else
			{
				std::cout << "{\"report\":{\"instrument:\"" << sampleParams->getInstrument() << "\"invalid\"}" << std::endl;
				bWasError = true;
			}
		}
		else
		{
			std::cout << "{\"status\":{\"message\":\"No valid accounts\",\"state\":\"failed\"}" << std::endl;

			bWasError = true;
		}
		session->unsubscribeResponse(responseListener);
		responseListener->release();
		logout(session, sessionListener);
	}
	else
	{
		bWasError = true;
	}

	session->unsubscribeSessionStatus(sessionListener);
	sessionListener->release();
	session->release();

	delete loginParams;
	delete sampleParams;

	if (bWasError)
		return -1;
	return 0;
}

IO2GRequest *createEntryOrderRequest(IO2GSession *session, const char *sOfferID, const char *sAccountID, int iAmount, double dRate, const char *sBuySell, const char *sOrderType, const char *expDate)
{
	O2G2Ptr<IO2GRequestFactory> requestFactory = session->getRequestFactory();
	if (!requestFactory)
	{
		std::cout << "{\"status\":{\"message\":\"Cannot create request factory\",\"state\":\"failed\"}" << std::endl;


		return NULL;
	}
	O2G2Ptr<IO2GValueMap> valuemap = requestFactory->createValueMap();
	valuemap->setString(Command, O2G2::Commands::CreateOrder);
	valuemap->setString(OrderType, sOrderType);
	valuemap->setString(AccountID, sAccountID);
	valuemap->setString(OfferID, sOfferID);
	valuemap->setString(BuySell, sBuySell);
	valuemap->setInt(Amount, iAmount);
	valuemap->setDouble(Rate, dRate);
	valuemap->setString(CustomID, "EntryOrder");

	if (strcmp(expDate, "") != 0)
	{
		valuemap->setString(TimeInForce, O2G2::TIF::GTD); // 'Good till Date'
		valuemap->setString(ExpireDateTime, expDate); //  FIX UTCTimestamp format: "yyyyMMdd-HH:mm:ss.SSS" (milliseconds are optional)
	}

	O2G2Ptr<IO2GRequest> request = requestFactory->createOrderRequest(valuemap);
	if (!request)
	{
		std::cout << "{\"status\":{\"message\":\"Error " << requestFactory->getLastError() << "\",\"state\":\"failed\"}" << std::endl;

		//std::cout << requestFactory->getLastError() << std::endl;
		return NULL;
	}
	return request.Detach();
}

// Determine order type based on parameters: current market price of a trading instrument, desired order rate, order direction
std::string getEntryOrderType(double dBid, double dAsk, double dRate, const char *sBuySell, double dPointSize, int iCondDistLimit, int iCondDistStop)
{
	double dAbsoluteDifference = 0.0;
	if (strcmp(sBuySell, O2G2::Buy) == 0)
	{
		dAbsoluteDifference = dRate - dAsk;
	}
	else
	{
		dAbsoluteDifference = dBid - dRate;
	}
	int iDifferenceInPips = roundPrice(dAbsoluteDifference / dPointSize);

	if (iDifferenceInPips >= 0)
	{
		if (iDifferenceInPips <= iCondDistStop)
		{
			std::cout << "{\"report\":{\"message\":\"Price is too close to market.\"}" << std::endl;
			return NULL;
		}
		return O2G2::Orders::StopEntry;
	}
	else
	{
		if (-iDifferenceInPips <= iCondDistLimit)
		{
			std::cout << "{\"report\":{\"message\":\"Price is too close to market.\"}" << std::endl;
			return NULL;
		}
		return O2G2::Orders::LimitEntry;
	}
}

void printSampleParams(std::string &sProcName, LoginParams *loginParams, SampleParams *sampleParams)
{
	std::string outMsg = "{\"request\":{\"message\":\"Running " + sProcName + "\"},";

	std::cout << outMsg << std::endl;
	//"{\"report\":{\"message\":\"Running " << sProcName << " with arguments:" << std::endl;

	std::string outCommon = " ";// = "";


	std::cout << "{\"requestinfo\":" << std::endl;

	// Login (common) information
	if (loginParams)
	{
		/*	outCommon =

				(
					loginParams->getLogin() + ","
				+ loginParams->getURL() + ","
				+ loginParams->getConnection() + ","
				+ loginParams->getSessionID() + ","
				+ loginParams->getPin() + ","
				+ "}"
					);*/
					//std::cout <<outCommon<< std::endl;

		std::cout << "{\"common\":{"
		 << "\"login\":\""	<< loginParams->getLogin() << "\","
		 << "\"url\":\""	<< loginParams->getURL() << "\","
		 << "\"connection\":\""	<< loginParams->getConnection() << "\","
		 << "\"sessionid\":\""	<< loginParams->getSessionID() << "\","
		 << "\"pin\":\""	<< loginParams->getPin() << "\""
			<< "},"
			<< std::endl;
	}

	std::string outSpecific = "";
	// Sample specific information
	if (sampleParams)
	{
		std::cout << "{\"specific\":{"
		 << "\"Instrument\":\""	  << sampleParams->getInstrument() << "\","
		 << "\"BuySell\":\""		  << sampleParams->getBuySell() <<  "\","
		 << "\"Rate\":\""		  << sampleParams->getRate() << "\","
		 << "\"Lots\":\""			  << sampleParams->getLots() <<  "\","
		 << "\"Account\":\""		  << sampleParams->getAccount() << "\","
		 << "\"ExpireDate\":\""			  << sampleParams->getExpDate() <<  "\""
			<< "}"
			<< std::endl;
	}
	std::cout << "}" << std::endl;
}

void printHelp(std::string &sProcName)
{
	std::cout << sProcName << " sample parameters:" << std::endl << std::endl;

	std::cout << "/login | --login | /l | -l" << std::endl;
	std::cout << "Your user name." << std::endl << std::endl;

	std::cout << "/password | --password | /p | -p" << std::endl;
	std::cout << "Your password." << std::endl << std::endl;

	std::cout << "/url | --url | /u | -u" << std::endl;
	std::cout << "The server URL. For example, http://www.fxcorporate.com/Hosts.jsp." << std::endl << std::endl;

	std::cout << "/connection | --connection | /c | -c" << std::endl;
	std::cout << "The connection name. For example, \"Demo\" or \"Real\"." << std::endl << std::endl;

	std::cout << "/sessionid | --sessionid " << std::endl;
	std::cout << "The database name. Required only for users who have accounts in more than one database. Optional parameter." << std::endl << std::endl;

	std::cout << "/pin | --pin " << std::endl;
	std::cout << "Your pin code. Required only for users who have a pin. Optional parameter." << std::endl << std::endl;

	std::cout << "/instrument | --instrument | /i | -i" << std::endl;
	std::cout << "An instrument which you want to use in sample. For example, \"EUR/USD\"." << std::endl << std::endl;

	std::cout << "/buysell | --buysell | /d | -d" << std::endl;
	std::cout << "The order direction. Possible values are: B - buy, S - sell." << std::endl << std::endl;

	std::cout << "/rate | --rate | /r | -r" << std::endl;
	std::cout << "Desired price of an entry order." << std::endl << std::endl;

	std::cout << "/lots | --lots " << std::endl;
	std::cout << "Trade amount in lots. Optional parameter." << std::endl << std::endl;

	std::cout << "/account | --account " << std::endl;
	std::cout << "An account which you want to use in sample. Optional parameter." << std::endl << std::endl;

	std::cout << "/expireDate | --expireDate " << std::endl;
	std::cout << "The expire date for entry order. Optional parameter" << std::endl << std::endl;
}

bool checkObligatoryParams(LoginParams *loginParams, SampleParams *sampleParams)
{
	/* Check login parameters. */
	if (strlen(loginParams->getLogin()) == 0)
	{
		std::cout << LoginParams::Strings::loginNotSpecified << std::endl;
		return false;
	}
	if (strlen(loginParams->getPassword()) == 0)
	{
		std::cout << LoginParams::Strings::passwordNotSpecified << std::endl;
		return false;
	}
	if (strlen(loginParams->getURL()) == 0)
	{
		std::cout << LoginParams::Strings::urlNotSpecified << std::endl;
		return false;
	}
	if (strlen(loginParams->getConnection()) == 0)
	{
		std::cout << LoginParams::Strings::connectionNotSpecified << std::endl;
		return false;
	}

	/* Check other parameters. */
	if (strlen(sampleParams->getInstrument()) == 0)
	{
		std::cout << SampleParams::Strings::instrumentNotSpecified << std::endl;
		return false;
	}
	if (strlen(sampleParams->getBuySell()) == 0)
	{
		std::cout << SampleParams::Strings::buysellNotSpecified << std::endl;
		return false;
	}
	if (isNaN(sampleParams->getRate()))
	{
		std::cout << SampleParams::Strings::rateNotSpecified << std::endl;
		return false;
	}
	if (sampleParams->getLots() <= 0)
	{
		std::cout << "'Lots' value " << sampleParams->getLots() << " is invalid" << std::endl;
		return false;
	}

	return true;
}

