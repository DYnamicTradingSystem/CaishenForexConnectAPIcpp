#include "stdafx.h"
#include "ResponseListener.h"
#include "SessionStatusListener.h"
#include "LoginParams.h"
#include "SampleParams.h"
#include "CommonSources.h"

void printHelp(std::string &);
bool checkObligatoryParams(LoginParams *, SampleParams *);
void printSampleParams(std::string &, LoginParams *, SampleParams *);
double calculateRate(const char *, const char *, double, double, double);
IO2GRequest *createEntryOrderRequest(IO2GSession *, const char *, const char *, int, double, const char *, const char *);
void findOrder(IO2GSession *, const char *, const char *, ResponseListener *);

int main(int argc, char *argv[])
{
	std::string procName = "CreateAndFindEntry";
	std::cout << "{\"process\":{\"state\":\"starting\"}}" << std::endl;

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
				std::cout << "{\"account\":{\"id\":\""
					<< sampleParams->getAccount() << "\"}}" << std::endl;
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

					double dRate = calculateRate(sampleParams->getOrderType(), sampleParams->getBuySell(), offer->getAsk(), offer->getBid(), offer->getPointSize());
					O2G2Ptr<IO2GRequest> request = createEntryOrderRequest(session, offer->getOfferID(), sampleParams->getAccount(),
						iAmount, dRate, sampleParams->getBuySell(), sampleParams->getOrderType());
					if (request)
					{
						responseListener->setRequestID(request->getRequestID());
						session->sendRequest(request);

						if (responseListener->waitEvents())
						{
							std::string sOrderID = responseListener->getOrderID();
							if (!sOrderID.empty())
							{
								std::cout << "{\"orderresult\":{\"message\":\"You have successfully created an entry order for instrument \","
									<< sampleParams->getInstrument() << std::endl;
								std::cout << "\"orderid\":\"" << sOrderID << "\"}" << std::endl;
								findOrder(session, sOrderID.c_str(), sampleParams->getAccount(), responseListener);

								std::cout << "{\"process\":{\"state\":\"done,success\"}}" << std::endl;
							}
						}
						else
						{
							std::cout << "Response waiting timeout expired" << std::endl;
							bWasError = true;
						}
					}
					else
					{
						std::cout << "Cannot create request" << std::endl;
						bWasError = true;
					}
				}
				else
				{
					std::cout << "Cannot get login rules" << std::endl;
					bWasError = true;
				}
			}
			else
			{
				std::cout << "The instrument '" << sampleParams->getInstrument() << "' is not valid" << std::endl;
				bWasError = true;
			}
		}
		else
		{
			std::cout << "No valid accounts" << std::endl;
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

// For the purpose of this example we will place entry order 10 pips from the current market price
double calculateRate(const char *sOrderType, const char *sBuySell, double dAsk, double dBid, double dPointSize)
{
	double dRate = 0.0;
	if (strcmp(sOrderType, O2G2::Orders::LimitEntry) == 0)
	{
		if (strcmp(sBuySell, O2G2::Buy) == 0)
		{
			dRate = dAsk - 10 * dPointSize;
		}
		else
		{
			dRate = dBid + 10 * dPointSize;
		}
	}
	else
	{
		if (strcmp(sBuySell, O2G2::Buy) == 0)
		{
			dRate = dAsk + 10 * dPointSize;
		}
		else
		{
			dRate = dBid - 10 * dPointSize;
		}
	}
	return dRate;
}

IO2GRequest *createEntryOrderRequest(IO2GSession *session, const char *sOfferID, const char *sAccountID, int iAmount, double dRate, const char *sBuySell, const char *sOrderType)
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
	O2G2Ptr<IO2GRequest> request = requestFactory->createOrderRequest(valuemap);
	if (!request)
	{
		std::cout << "{\"status\":{\"message\":\"Error " << requestFactory->getLastError() << "\",\"state\":\"failed\"}" << std::endl;

		return NULL;
	}
	return request.Detach();
}

// Find order by ID and print information about it
void findOrder(IO2GSession *session, const char *sOrderID, const char *sAccountID, ResponseListener *responseListener)
{
	O2G2Ptr<IO2GRequestFactory> requestFactory = session->getRequestFactory();
	if (!requestFactory)
	{
		std::cout << "Cannot create request factory" << std::endl;
		return;
	}
	O2G2Ptr<IO2GRequest> request = requestFactory->createRefreshTableRequestByAccount(Orders, sAccountID);
	if (!request)
	{
		std::cout << requestFactory->getLastError() << std::endl;
		return;
	}
	responseListener->setRequestID(request->getRequestID());
	session->sendRequest(request);
	if (!responseListener->waitEvents())
	{
		std::cout << "{\"status\":{\"message\":\"Response waiting timeout expired\",\"state\":\"failed\"}" << std::endl;

		return;
	}
	O2G2Ptr<IO2GResponse> response = responseListener->getResponse();
	if (response)
	{
		O2G2Ptr<IO2GResponseReaderFactory> responseFactory = session->getResponseReaderFactory();
		O2G2Ptr<IO2GOrdersTableResponseReader> ordersReader = responseFactory->createOrdersTableReader(response);
		for (int i = 0; i < ordersReader->size(); ++i)
		{
			O2G2Ptr<IO2GOrderRow> order = ordersReader->getRow(i);
			if (strcmp(sOrderID, order->getOrderID()) == 0)
			{
				std::cout
					<< "{\"OrderData\":{"
					<< "\"OrderID\":\"" << order->getOrderID() << "\","

					<< "AccountID='" << order->getAccountID() << "', "
					<< "\"Type\":\"" << order->getType() << "\","

					<< "\"Status\":\"" << order->getStatus() << "\","
					<< "\"OfferID\":\"" << order->getOfferID() << "\","
					<< "\"Amount\":\"" << order->getAmount() << "\","

					<< "\"BuySell\":\"" << order->getBuySell() << "\","
					<< "\"Rate\":\"" << order->getRate() << "\","
					<< "\"TimeInForce\":\"" << order->getTimeInForce() << "\"}}"

					<< std::endl;
				break;
			}
		}
	}
	else
	{
		std::cout << "{\"status\":{\"message\":\"Cannot get response\",\"state\":\"failed\"}" << std::endl;

		return;
	}
}

void printSampleParams(std::string &sProcName, LoginParams *loginParams, SampleParams *sampleParams)
{
	std::cout << "Running " << sProcName << " with arguments:" << std::endl;

	// Login (common) information
	if (loginParams)
	{
		std::cout 
			<< "{\"requestdata\":{"
			<< "{\"common\":{"
			<< "\"login\":\"" << loginParams->getLogin() << "\","
			<< "\"url\":\"" << loginParams->getURL() << "\","
			<< "\"connection\":\"" << loginParams->getConnection() << "\","
			<< "\"sessionid\":\"" << loginParams->getSessionID() << "\","
			<< "\"pin\":\"" << loginParams->getPin() << "\""
			<< "},"
			<< std::endl;
	}

	// Sample specific information
	if (sampleParams)
	{
		std::cout << "{\"specific\":{"
			<< "\"Instrument\":\"" << sampleParams->getInstrument() << "\","
			<< "\"BuySell\":\"" << sampleParams->getBuySell() << "\","
			<< "\"Rate\":\"" << sampleParams->getRate() << "\","
			<< "\"Lots\":\"" << sampleParams->getLots() << "\","
			<< "\"Account\":\"" << sampleParams->getAccount() << "\","
			<< "}"	
			<< "}"
			<< std::endl;

	}
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

	std::cout << "/account | --account " << std::endl;
	std::cout << "An account which you want to use in sample. Optional parameter." << std::endl << std::endl;

	std::cout << "/buysell | --buysell | /d | -d" << std::endl;
	std::cout << "The order direction. Possible values are: B - buy, S - sell." << std::endl << std::endl;

	std::cout << "/ordertype | --ordertype " << std::endl;
	std::cout << "Type of an entry order. Possible values are: LE - entry limit, SE - entry stop. Default value is LE. Optional parameter." << std::endl << std::endl;

	std::cout << "/lots | --lots " << std::endl;
	std::cout << "Trade amount in lots. Optional parameter." << std::endl << std::endl;
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
	if (strlen(sampleParams->getOrderType()) == 0)
	{
		sampleParams->setOrderType("LE"); // default
		std::cout << "OrderType='" << sampleParams->getOrderType() << "'" << std::endl;
	}
	else
	{
		if (strcmp(sampleParams->getOrderType(), O2G2::Orders::LimitEntry) != 0 &&
			strcmp(sampleParams->getOrderType(), O2G2::Orders::StopEntry) != 0)
		{
			sampleParams->setOrderType("LE"); // default
			std::cout << "OrderType='" << sampleParams->getOrderType() << "'" << std::endl;
		}
	}
	if (sampleParams->getLots() <= 0)
	{
		std::cout << "'Lots' value " << sampleParams->getLots() << " is invalid" << std::endl;
		return false;
	}

	return true;
}

