/*
* Assumption:
*   order id is unique across all order messages
*   ticker symbol are integer number only
*   provided input data with correct messages, not corrupted messages or varied character messages
*   in amend both price and size can be modified
* Decisions:
*   I didn't use the boost::multi_index container. It could have made my code simpler.
*   In the past I observed it has performance issues. So only STL provided containers.
*   prices are double, precision compare to 6 decimal places - epsilon_value = 1e-6;
*
* Improvement:
*   if we know the number of decimal places in the price, we can convert to integer, can do integer operations,
*   which are faster than floating point operation
*   We can use flat_map in C++23
*
* Note:
*   This code is built with C++20 enabled, using the MS Visual Studio compiler.
*   For readability and sharability I have placed all code in a single file,  MatchingEnginee2.cpp
*   removed all test cases while sharing the code
*/

#include <iostream>
#include <utility>
#include <deque>
#include <unordered_map>
#include <map>
#include <functional>
#include <unordered_set>

#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <iterator>
#include <ranges>
#include <cmath>
#include <cstdint>
//#include <gogole/gtest>

namespace util
{
	using PriceType = double;
	constexpr PriceType epsilon_value = 1e-6;
	inline bool is_zero(PriceType a)                         { return std::fabs(a) < epsilon_value; }
	inline bool are_equal(PriceType a, PriceType b)          { return std::fabs(a - b) < epsilon_value; }
	inline bool are_not_equal(PriceType lhs, PriceType rhs)  { return !are_equal(lhs, rhs); }
	inline bool less_than(PriceType a, PriceType b)          { return (b - a) > epsilon_value; }
	inline bool greater_than(PriceType a, PriceType b)       { return (a - b) > epsilon_value; }
	inline bool less_than_equal(PriceType a, PriceType b)    { return a <= (b + epsilon_value); }
	inline bool greater_than_equal(PriceType a, PriceType b) { return (a + epsilon_value) >= b; }

	std::function<bool(PriceType, PriceType)> book_comp_func[2] =
	{
		std::bind(greater_than_equal, std::placeholders::_1, std::placeholders::_2),
		std::bind(less_than_equal,    std::placeholders::_1, std::placeholders::_2)
	};
};
//uint64_t get_numeric(const std::string& str) { return std::hash<std::string>{}(str); }
uint64_t get_numeric( std::string str) { str.erase(0, 1);  return std::stoll(str); }
using namespace util;
struct Order
{
	uint64_t instr = 0;
	uint64_t qty = 0;
	char     side = 0;
	PriceType price = 0;
	uint64_t id = 0;
	std::string id_str;
	uint64_t executed_qty=0;
	Order(uint64_t inst, std::string ids, char s, uint64_t q, PriceType p)
		:instr(inst), id(get_numeric(ids)), qty(q), side(s), price(p), id_str(ids)
	{}
	uint64_t getQty() const { return qty - executed_qty; }
	friend std::ostream& operator <<(std::ostream& out, const Order& o);
};
std::ostream& operator <<(std::ostream& out, const Order& o)
{
	out << "OrderId:" << o.id_str << " Side:" << (o.side == 'B' ? "Buy " : "Sell") << " Quantity:" << o.getQty() << " Price:" << o.price << '\n';
	return out;
}
struct Comparator
{
	bool lesser = true;
	Comparator(bool val) : lesser(val) { }
	bool operator()(PriceType lhs, PriceType rhs) const { return lesser ? lhs < rhs : lhs > rhs; }
};
struct Level
{
	PriceType price = 0;
	std::deque<std::reference_wrapper<Order>> orders;
	Level(PriceType p) : price(p) { }
	Level() :price(0) { }
	void removeFromLevel(const Order& o)
	{
		auto ite = std::find_if(orders.begin(), orders.end(), [&o](const Order& other) { return o.id == other.id; });
		if (ite != orders.end())
			orders.erase(ite);
	}
	void addToLevel(Order& o)
	{
		orders.push_back(std::ref(o));
	}
};

using DepthType = std::map<PriceType, Level, Comparator>;

struct Instrument
{
private:
	std::unordered_map<uint64_t, Order> m_orders;
	DepthType m_BuyDepth, m_SellDepth;
	DepthType* mDepth[2] = { &m_BuyDepth, &m_SellDepth };

	uint64_t matchBook(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price);
	DepthType& getDepth(char side) { return side == 'B' ? m_BuyDepth : m_SellDepth; }
	auto findOrder(const std::string& orderid);
	auto removeFromDepth(DepthType& depth, PriceType price, Order& order);
	auto moveToEnd(DepthType& depth, PriceType price, Order& order);
public:
	uint64_t m_ticker = 0;
	Instrument(uint64_t t) : m_ticker(t), m_BuyDepth(false), m_SellDepth(true) { /*std::cout << "Inst:" << m_ticker << " created\n";*/ }
	bool addOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price);
	bool modOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price);
	bool canOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price);
	void printBook(const std::string& prefix, uint8_t max_levels = 5);
	void printDepth(DepthType& depth);

};
void logTrade(uint64_t ticker, const std::string& oid, uint64_t exec_q, PriceType exec_p, char side, const Order& opp_ord);

auto Instrument::findOrder(const std::string& orderid)
{
	auto numeric_orderid = get_numeric(orderid);
	auto ord_ite = m_orders.find(numeric_orderid);
	if (ord_ite != m_orders.end())
		return std::make_pair(true, ord_ite);
	return std::make_pair(false, m_orders.end());
}

auto Instrument::removeFromDepth(DepthType& depth, PriceType price, Order& ord)
{
	if (auto depth_ite = depth.find(ord.price); depth_ite != depth.end())
	{
		depth_ite->second.removeFromLevel(ord);
		if (depth_ite->second.orders.empty())
			depth.erase(depth_ite);
	}
}

auto Instrument::moveToEnd(DepthType& depth, PriceType price, Order& order)
{
	if (auto depth_ite = depth.find(ord.price); depth_ite != depth.end() && std::next(depth_ite) != depth.end() /* element alredy last in the queue*/)
	{
		std::rotate(depth_ite, depth_ite + 1, depth.end());
	}
}

bool Instrument::addOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price)
{
	auto numeric_orderid = get_numeric(orderid);
	if (m_orders.contains(numeric_orderid))
		return false;
	auto exec_qty = matchBook(ticker, orderid, side, quantity, price);
	quantity -= exec_qty;
	if (quantity <= 0) // new order fully executed;
		return true;

	auto res = m_orders.emplace(std::piecewise_construct, std::forward_as_tuple(numeric_orderid), std::forward_as_tuple(ticker, orderid, side, quantity, price));
	getDepth(side)[price].addToLevel(res.first->second);

	return true;
}
bool Instrument::modOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price)
{
	auto res = findOrder(orderid);
	if (res.first == false)
	{
		std::cerr << ticker << ' ' << orderid << " not found\n";
		return false;
	}
	auto& ord = res.second->second;
	if (quantity <= ord.executed_qty)
	{
		std::cerr << ticker << " amended quantity less than executed quantity\n";
		return false;
	}
	ord.qty = quantity;
	quantity = ord.getQty();
	if (are_not_equal(price, ord.price)) // depth level change to price change
	{
		auto &depth = getDepth(side);
		removeFromDepth(depth, ord.price, ord);

		ord.price = price;
		auto exec_qty = matchBook(ticker, orderid, side, quantity, price);
		quantity -= exec_qty;
		ord.executed_qty += exec_qty;
		if (quantity <= 0) // on mod, order fully executed
			m_orders.erase(res.second);
		else
			depth[ord.price].addToLevel(ord);
	}
	else //only quantity changed, so execution priority changed, so move to end
	{
		moveToEnd(depth, ord.price, ord);
	}

	return true;
}
bool Instrument::canOrder(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price)
{
	auto res = findOrder(orderid);
	if (res.first == false)
	{
		std::cerr << ticker << ' ' << orderid << " not found\n";
		return false;
	}

	auto& ord = res.second->second;
	auto &depth = getDepth(side);
	removeFromDepth(depth, ord.price, ord);
	m_orders.erase(res.second);
	return true;
}

uint64_t Instrument::matchBook(uint64_t ticker, const std::string& orderid, char side, uint64_t quantity, PriceType price)
{
	uint8_t index = side == 'B' ? 0 : 1;
	auto &oppdepth = mDepth[!index];
	if (oppdepth->empty())
		return 0;

	uint64_t total_executed_qty = 0;
	uint64_t remaining_qty = quantity;
	for (auto opp_ite = oppdepth->begin(); opp_ite != oppdepth->end() && book_comp_func[index](price, opp_ite->first) && remaining_qty; )
	{
		uint64_t executed_qty = 0;
		auto level_orders = &opp_ite->second.orders;
		for (auto ord_ite = level_orders->begin(); ord_ite != level_orders->end() && remaining_qty; )
		{
			auto cur_exe_qty = std::min(ord_ite->get().qty, remaining_qty);
			//auto cur_exe_qty = std::min(ord_ite->get().qty - ord_ite->get().executed_qty, remaining_qty);
			remaining_qty -= cur_exe_qty;
			ord_ite->get().qty -= cur_exe_qty;
			executed_qty += cur_exe_qty;
			logTrade(ticker, orderid, cur_exe_qty, ord_ite->get().price, side, *ord_ite);
			if (ord_ite->get().qty == 0) // erase the order, fully exeucted order
			{
				m_orders.erase(ord_ite->get().id);
				ord_ite = level_orders->erase(ord_ite);
			}
			else
			{
				ord_ite->get().executed_qty += cur_exe_qty;
			}
			if (level_orders->empty()) // if all orders of the level executed, remove cur level info from the depth info
			{
				opp_ite = oppdepth->erase(opp_ite);
				break;
			}
		}
		total_executed_qty += executed_qty;
	}
	return total_executed_qty;
}

void logTrade(uint64_t ticker, const std::string& oid, uint64_t exec_q, PriceType exec_p, char side, const Order& opp_ord)
{
	std::cout << "Trade: " << ticker << ' ' << oid << ' ' << exec_q << ' ' << exec_p << ' ' << side << '\n';
	std::cout << "       " << ticker << ' ' << opp_ord.id_str << ' ' << exec_q << ' ' << exec_p << ' ' << opp_ord.side << '\n';
}
void Instrument::printBook(const std::string& prefix, uint8_t max_levels)
{
	auto print = [&prefix] (auto &book, uint8_t level)
	{
		int count = 1;
		for (auto book_ite = book.begin(); book_ite != book.end() && level; ++book_ite, --level)
		{
			for (auto order_ite = book_ite->second.orders.begin(); order_ite != book_ite->second.orders.end(); ++order_ite)
			{
				std::cout<< prefix<< * order_ite;
			}
		}
	};
	print(getDepth('B'), max_levels);
	print(getDepth('S'), max_levels);
}
void Instrument::printDepth(DepthType& book)
{
	for (auto book_ite = book.begin(); book_ite != book.end(); ++book_ite)
	{
		std::cout << "Price " << book_ite->first << " --> ";
		for (auto order_ite = book_ite->second.orders.begin(); order_ite != book_ite->second.orders.end(); ++order_ite)
			std::cout <<order_ite->get().id_str<<' ';
		std::cout << '\n';
	}
}

class MarketDataHandler
{
	std::unordered_map<uint64_t, Instrument> m_instruments;
	Instrument* addInstrument(uint64_t ticker);
public:
	Instrument* getInstrument(uint64_t);
	void processMessage(uint64_t ticker, char action, const std::string& orderid, char side, uint64_t quantity, PriceType price);
	auto getInstruments() { return m_instruments; }
};

Instrument* MarketDataHandler::addInstrument(uint64_t ticker)
{
	auto res = m_instruments.emplace(ticker, ticker);
	return &res.first->second;
}
Instrument* MarketDataHandler::getInstrument(uint64_t numericticker)
{
	auto ite = m_instruments.find(numericticker);
	if (ite != m_instruments.end())
		return &ite->second;
	return nullptr;
}

void MarketDataHandler::processMessage(uint64_t ticker, char action, const std::string& orderid, char side, uint64_t quantity, PriceType price)
{
	auto instr = getInstrument(ticker);
	switch (action)
	{
	case 'N':
		if (!instr)
			instr = addInstrument(ticker);
		instr->addOrder(ticker, orderid, side, quantity, price);
		break;
	case 'A':
		if (instr)
			instr->modOrder(ticker, orderid, side, quantity, price);
		break;
	case 'C':
		if (instr)
			instr->canOrder(ticker, orderid, side, quantity, price);
		break;
	default:
		std::cerr << "Unknown action " << action << '\n';
	}
}

class CSVFile
{

public:
	std::vector<std::vector<std::string>> messages;
	void readFile(const std::string& filename);
	auto csvSplit(const std::string& str, const std::string& delim = ",");
	void print();
	void addData();
};
auto  CSVFile::csvSplit(const std::string& str, const std::string& delim)
{
	std::vector< std::string> vec;
	auto res = str | std::views::split(delim);
	for (auto w : res)
		vec.emplace_back(std::string(w.begin(), w.end()));
	return vec;
}
void  CSVFile::readFile(const std::string& filename)
{
	std::ifstream in_file(filename);
	if (!in_file.is_open())
	{
		std::cerr << "Failed to open file\n";
		return;
	}

	std::vector<std::string> vec; vec.assign(std::istream_iterator<std::string>(in_file), std::istream_iterator<std::string>());
	for (auto& msg : vec)
		messages.emplace_back(csvSplit(msg));
	std::cout << '\n';
}
void  CSVFile::print()
{
	for (auto msg : messages)
	{
		for (auto field : msg)
			std::cout << field << ' ';
		std::cout << '\n';
	}
}
void CSVFile::addData()
{
	messages.emplace_back(csvSplit("101,N,A001,S,3000,6.8"));
	messages.emplace_back(csvSplit("101,N,A002,S,1000,6.9"));
	messages.emplace_back(csvSplit("101,N,A003,B,2000,6.7"));
	messages.emplace_back(csvSplit("101,N,A004,B,1000,6.8"));
	messages.emplace_back(csvSplit("101,A,A003,B,2000,6.9"));
	messages.emplace_back(csvSplit("102,N,A005,S,2000,10.2"));
	messages.emplace_back(csvSplit("102,N,A006,B,2000,10.1"));
	messages.emplace_back(csvSplit("102,N,A007,B,2000,10.1"));
	messages.emplace_back(csvSplit("102,C,A006,B,2000,10.1"));
}
int main(int argc, char** argv)
{
	std::cout << "Matching Engine START\n";
	std::string filename = "D:\\Devel\\C++\\MatchingEngine\\ordmsg.txt";
	CSVFile csvfile;
	if (argc > 1)
	{
		filename = argv[1];
		std::cout << "Reading data from file " << filename << '\n';
		csvfile.readFile(filename);
		if (csvfile.messages.empty())
		{
			std::cerr << "No messages to procees in file\n";
			return -1;
		}
	}
	else
	{
		std::cout << "Processing default initialized messages, you can pass file name as argument to process\n";
		csvfile.addData();
	}
	//csvfile.print();
	std::unique_ptr< MarketDataHandler> handler = std::make_unique<MarketDataHandler>();
	for (auto msg : csvfile.messages)
	{
		try
		{

			//101,N,A001,S,3000,6.8
			uint64_t ticker = std::stoll(msg[0]);
			char action = msg[1][0];
			std::string orderid = msg[2];
			char side = msg[3][0];
			int64_t quantity = std::stoll(msg[4]);
			PriceType price = std::stod(msg[5]);
			if (quantity <= 0 || less_than_equal(price, 0))
			{
				std::cerr << "Invalid price/size \n";
				continue;
			}
			handler->processMessage(ticker, action, orderid, side, quantity, price);
		}
		catch (std::exception &e)
		{
			std::cerr << "Excepton thrown while processing elements, error "<<e.what()<<'\n';
		}
	}
	for (auto [key, instr] : handler->getInstruments())
	{
		std::cout << "Ticker:" << instr.m_ticker << '\n';
		instr.printBook("", 5);
		std::cout << "\n";
	}
	std::cout << "Matching Engine END\n";
	return 0;
}
