#include <condition_variable>
#include <iostream>
#include <cstdint>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
/* Assumption:
  * It is single thread and sequential. So lock and queues are  NOT used
  * Computed TheoriticalPrice comes after the bid and offer price received
  * On every TheoriticalPrice update quoted_bid and quoted_ask price should be computed and order sent to market, based on the condition
  * quoted_bid < Theoriticalprice < quoted_ask
  * ticksize is same across whole price range
  * Here is single instrument, so classes are not decomposed as part of the class hierarchy
  * we don't send multiple orders at the same side, at a time there is only one  buy and sell order at each side
  * sequence of order ack is guaranteed as per the sequence of order sent
  * There is no market bid volume and ask volume info,  so while cancelling and inserting the order, we don't know are the only one in the market or many on the same price
  * Only one set of orders, so event queue not used
  * On theo update, both bid and ask quote price changes
  * It is quoting, so orders exist on both sides or none
*/

namespace tick_util
{
    constexpr double epsilon_value = 1e-6;
    bool is_zero(double a )                     { return std::fabs(a) < epsilon_value; }
    bool are_equal(double a, double b)          { return std::fabs(a - b) < epsilon_value; }
    bool less_than(double a, double b)          { return (b - a) > epsilon_value; }
    bool greater_than(double a, double b)       { return (a - b) > epsilon_value; }
    bool less_than_equal(double a, double b)    { return a <= (b + epsilon_value); }
    bool greater_than_equal(double a, double b) { return (a + epsilon_value) >= b; }

    double getDownTickPrice(double price, double tick) { return std::floor( price / tick + epsilon_value) * tick; }
    double getUpTickPrice  (double price, double tick) { return std::ceil(price / tick - epsilon_value ) * tick ; }
}
using namespace tick_util;
class InstrumentQuoter;

bool running = true;
class Execution
{
    std::deque<std::pair<uint32_t, uint8_t>> m_events;
    InstrumentQuoter *m_observer = nullptr;
    std::mutex cv_mutex;
    std::condition_variable cv;
public:
  void requestOrderAdd( uint32_t id, std::string const &feedcode, char orderSide, double orderPrice, uint32_t orderVolume)
  {
    {
        std::lock_guard lock(cv_mutex);
        std::cout<<"New Order "<<id<<' '<<orderSide<<' '<<orderPrice<<' '<<orderVolume<<'\n';
        m_events.emplace_back(id, 1);
    }cv.notify_one();
  }
  void requestOrderRemove( uint32_t id)
  { 
    {
        std::lock_guard lock(cv_mutex);
        std::cout<<"Can Order "<<id<<'\n'; 
        m_events.emplace_back(id, 2);
    }cv.notify_one();
  }
  void setObserver(InstrumentQuoter *obs) { m_observer = obs; }
  Execution();
};

enum Status
{
    DONE,
    NEW=1,
    ACKED,
    PENDING_NEW,
    PENDING_CANCEL,
};
enum Action { NEW_ORDER, CANCEL_ORDER, NO_ACTION};
struct Order
{
    uint32_t id;
    char side;
    double price=0;
    Status status;
    bool send_new_order_on_cancel_ack=false;
    bool send_cancel_order_on_new_order_ack = false;
    void reset() 
    {
        price = 0;
        status = DONE;
        send_new_order_on_cancel_ack = send_cancel_order_on_new_order_ack= false;
    }
};

class InstrumentQuoter
{
    double m_quoteoffset;
    double m_quotevolume;
    double m_tickwidth;
    Execution  &m_execution;
    std::string m_feedcode;

    double m_market_bid=0, m_market_ask=0,m_theo=0;
    double m_quote_bid=0, m_quote_ask=0;
    uint32_t getOrderId() { static uint32_t m_order_id = 0; return ++m_order_id; }
    bool isBidQuoted() { return greater_than(m_quote_bid, 0); }
    bool isAskQuoted() { return greater_than(m_quote_ask, 0); }
    bool isValidBidPrice(double bid);
    bool isValidAskPrice(double bid);
    bool isQuoteExist(const Order &order) { return order.status >= ACKED; }

    double getBidQuotePrice(double theo) { return getDownTickPrice(theo - m_quoteoffset, m_tickwidth); }
    double getAskQuotePrice(double theo) { return getUpTickPrice  (theo + m_quoteoffset, m_tickwidth); }
    double getOwnBidQuotedPrice() { return (m_bid_order.status >= ACKED) ? m_bid_order.price : 0; }
    double getOwnAskQuotedPrice() { return (m_ask_order.status >= ACKED) ? m_ask_order.price : 0; }

    bool isBidOrder(uint32_t id) { return id == m_bid_order.id; }
    bool isBidOrder(char side) { return side == 'B'? true: false; }

    Order m_bid_order, m_ask_order;
    void sendOrder(Order &order, Action action);
    void removeQuote(Order &order);
    void addNewQuote(Order &order);
    void addEvent(Action action, char side, double price);
    void consumeEvent();
public:
  InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution);
  void OnTheoreticalPrice( double theoreticalPrice);
  void OnBestBidOffer( double bidPrice, double offerPrice);
  void OnOrderAddConfirm( uint32_t id);
  void OnOrderRemoveConfirm( uint32_t id);
  void notify(uint32_t id, uint8_t action);
};

InstrumentQuoter::InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution)
    :m_quoteoffset(quoteOffset), m_quotevolume(quoteVolume), m_tickwidth(tickWidth), m_execution(execution), m_feedcode(feedcode)
{
    std::cout<<"InstrumentQuoter created on symbol "<<m_feedcode<<'\n';

    m_bid_order.side = 'B';
    m_bid_order.status= DONE;

    m_ask_order.side = 'S';
    m_ask_order.status= DONE;
}
void InstrumentQuoter::sendOrder(Order &order, Action action)
{
    if( action == NEW_ORDER && order.status == NEW )
    {
        order.id=getOrderId();
        order.status = PENDING_NEW;
        m_execution.requestOrderAdd(order.id, m_feedcode, order.side, order.price, m_quotevolume);
    }
    else if ( action == CANCEL_ORDER && order.status == ACKED )
    {
        order.status = PENDING_CANCEL;
        m_execution.requestOrderRemove(order.id);
    }
}

void InstrumentQuoter::OnOrderAddConfirm(uint32_t id)
{
    Order &order = isBidOrder(id) ? m_bid_order : m_ask_order;
    std::cout<<"New Ack "<<order.id<<' '<<order.price<<' '<<order.side<<'\n';
    order.status = ACKED;
    if(order.send_cancel_order_on_new_order_ack)
    {
       order.send_cancel_order_on_new_order_ack = false;
       sendOrder(order, CANCEL_ORDER);
    }
}

void InstrumentQuoter::OnOrderRemoveConfirm( uint32_t id)
{
    Order &order = isBidOrder(id) ? m_bid_order : m_ask_order;
    std::cout<<"Can Ack "<<order.id<<' '<<order.price<<' '<<order.side<<'\n';

    if( order.send_new_order_on_cancel_ack )
    {
        if ( isBidOrder(order.side))
        {
            if ( is_zero(m_market_ask) || less_than(order.price, m_market_ask))
            {
                order.send_new_order_on_cancel_ack = false;
                order.status = NEW;
                sendOrder(order, NEW_ORDER);
                return;
            }
        }
        else 
        {
            if( is_zero(m_market_bid) || greater_than(order.price, m_market_bid) )
            {
                order.send_new_order_on_cancel_ack = false;
                order.status = NEW;
                sendOrder(order, NEW_ORDER);
                return;
            }
        }
    }
    //order.status = DONE;
    order.reset();
}
void InstrumentQuoter::OnBestBidOffer( double bidPrice, double offerPrice)
{
    m_market_bid = bidPrice;
    m_market_ask = offerPrice;
}
bool InstrumentQuoter::isValidBidPrice( double price)
{
    if( is_zero(price))
        return false;

    double quoted_bid = getOwnBidQuotedPrice();
    if( !is_zero(quoted_bid) && are_equal(price, quoted_bid)) // new price is same as prev price
        return false;
    if( !is_zero(m_market_ask) && greater_than_equal(price,  m_market_ask))
        return false;
    //I don't need to check again my own ask, because, i always cancel both and send
    //double quoted_ask = getOwnAskQuotedPrice();
    //if( !is_zero(m_quote_ask) && greater_than(price, quoted_ask))
    //    return false;
    return true;
}

bool InstrumentQuoter::isValidAskPrice( double price)
{
    if( is_zero(price))
        return false;

    double quoted_ask = getOwnAskQuotedPrice();
    if( !is_zero(quoted_ask) && are_equal(price, quoted_ask))
        return false;
    if( !is_zero(m_quote_bid) && less_than_equal(price,  m_market_bid))
        return false;
    return true;
}

void InstrumentQuoter::removeQuote(Order &order)
{
    if ( order.status == ACKED )
    {
        sendOrder(order, CANCEL_ORDER);
    }
    else if ( order.status  == PENDING_NEW )
    {
        order.send_cancel_order_on_new_order_ack = true;
        order.send_new_order_on_cancel_ack = false;
    }
}

void InstrumentQuoter::addNewQuote(Order &order)
{
    if ( order.status == ACKED )
    {
        sendOrder(order, CANCEL_ORDER);
        order.send_new_order_on_cancel_ack = true;
    }
    else if ( order.status  == PENDING_NEW )
    {
        order.send_cancel_order_on_new_order_ack = true;
        order.send_new_order_on_cancel_ack = true;
    }
    else if ( order.status == PENDING_CANCEL)
    {
        order.send_new_order_on_cancel_ack = true;
    }
    else if ( order.status == DONE )
    {
        order.status = NEW;
        sendOrder(order, NEW_ORDER);
    }
}

void InstrumentQuoter::OnTheoreticalPrice(double theoreticalPrice)
{
    m_theo = theoreticalPrice;
    std::cout<<"Theo Update "<<m_theo<<'\n';
    if( is_zero(m_theo) )
    {   
        std::cout<<"The reset cancelling bid/ask orders if any\n";
        removeQuote(m_bid_order);
        removeQuote(m_ask_order);
        return;
    }

    double bid = getBidQuotePrice(m_theo);
    double ask = getAskQuotePrice(m_theo); 
    if( greater_than_equal(bid, ask) )
    {
        std::cerr<<"Error in the theo price calcuation "<< bid<< ">=" << ask <<" on theo "<<m_theo<<'\n';
        return;
    }

    bool bid_valid = isValidBidPrice(bid);
    bool ask_valid = isValidAskPrice(ask);

    bool bid_not_sent=true, ask_not_sent=true;

    if ( bid_valid && ask_valid )
    {
        double prev_ask = m_ask_order.price;

        m_bid_order.price = bid;
        m_ask_order.price = ask;

        // based on the price movement change the order of remove and add quote
        if ( !is_zero(prev_ask) && greater_than_equal(bid,prev_ask) ) //shift up the quote
        {
            addNewQuote(m_ask_order);
            addNewQuote(m_bid_order);
        }
        else //shift down the quote
        {
            addNewQuote(m_bid_order);
            addNewQuote(m_ask_order);
        }
        return;
    }
}

Execution::Execution()
{
    return;
    std::thread([this]()
    {
        decltype(m_events) tmp;
        while( running )
        {
            tmp.clear();
            {
                std::unique_lock lock(cv_mutex);
                cv.wait(lock, [this]{return !m_events.empty();});
                tmp.swap(m_events);
            }
            for(auto event: tmp)
            {
                m_observer->notify(event.first, event.second);
            }
        }
        for(auto event: m_events)
            m_observer->notify(event.first, event.second);
    }).detach();
}

void InstrumentQuoter::notify(uint32_t id, uint8_t action)
{ 
    if( action == 1) 
        OnOrderAddConfirm(id); 
    else 
        OnOrderRemoveConfirm(id); 
}

int main()
{
    Execution handle;
    InstrumentQuoter iq("0001.HK", 0.20, 100, 0.05, handle);
    iq.OnBestBidOffer(9.80,10.20);
    iq.OnTheoreticalPrice(10);
    iq.OnOrderAddConfirm(1); iq.OnOrderAddConfirm(2);
    iq.OnTheoreticalPrice(9.950);
    iq.OnTheoreticalPrice(10.1);
    iq.OnOrderRemoveConfirm(1); iq.OnOrderRemoveConfirm(2);
    iq.OnOrderAddConfirm(3); iq.OnOrderAddConfirm(4);
    return 0;
}
