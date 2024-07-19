#include <chrono>
#include <condition_variable>
#include <iostream>
#include <cstdint>
#include <cmath>
#include <deque>
#include <mutex>
#include <ostream>
#include <thread>
#include <unordered_map>
#include <iomanip>
/* Assumption:
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
  * It is quoting, so while sending ordres send on both sides OR none
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

std::atomic<bool> running = true;


std::string getTime()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto us = duration_cast<microseconds>(now.time_since_epoch()) % 1000000;
    auto timer = system_clock::to_time_t(now);
    std::tm bt = *std::localtime(&timer);

    std::ostringstream oss;
    oss << std::put_time(&bt, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(6)<<us.count()<<' ';
    return oss.str();
}
class Execution
{
    std::deque<std::pair<uint32_t, uint8_t>> m_events;
    InstrumentQuoter *m_observer = nullptr;
    std::mutex cv_mutex;
    std::condition_variable cv;
public:
    bool i_am_done = false;
  void requestOrderAdd( uint32_t id, std::string const &feedcode, char orderSide, double orderPrice, uint32_t orderVolume)
  {
    {
        std::lock_guard lock(cv_mutex);
        std::cout<<getTime()<<"Exch:New "<<id<<' '<<orderSide<<' '<<orderPrice<<' '<<orderVolume<<" PENDING_ACK"<<'\n';
        m_events.emplace_back(id, 1);
    }cv.notify_one();
  }
  void requestOrderRemove( uint32_t id)
  { 
    {
        std::lock_guard lock(cv_mutex);
        std::cout<<getTime()<<"Exch:Can "<<id<<" PENDING_CANCEL"<<'\n'; 
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

enum class QuoteEvent : uint8_t 
{
    THEO_UPDATE=1,
    THEO_ERROR,
    DEPTH_UPDATE,
    BID_ORDER_ACKED,
    ASK_ORDER_ACKED,
    BID_ORDER_CANCELED,
    ASK_ORDER_CANCELED,
    NONE_EVENT
};

struct Order
{
    uint32_t id;
    char side;
    double price=0;
    Status status;
    bool send_cancel_order_on_new_order_ack = false;
    QuoteEvent notify_event= QuoteEvent::NONE_EVENT;
    void reset() 
    {
        price = 0;
        status = DONE;
        send_cancel_order_on_new_order_ack= false;
        notify_event = QuoteEvent::NONE_EVENT;
    }
    friend std::ostream& operator<<(std::ostream& out, const Order &order);
};

std::ostream& operator<< (std::ostream& out, const Order &order)
{
    out<<order.id<<'\t'<<(char)order.side<<' '<<order.price<<' '<<order.status;
    return out;
}

class InstrumentQuoter
{
    double m_quoteoffset;
    double m_quotevolume;
    double m_tickwidth;
    Execution  &m_execution;
    std::string m_feedcode;

    double m_market_bid=0, m_market_ask=0,m_theo=0;
    double m_computed_bid=0, m_computed_ask=0;

    Order m_bid_order, m_ask_order;

    uint32_t getOrderId() { static uint32_t m_order_id = 0; return ++m_order_id; }
    bool willHitMarket(const Order &order);

    double getOwnBidQuotedPrice() { return (m_bid_order.status >= ACKED) ? m_bid_order.price : 0; }
    double getOwnAskQuotedPrice() { return (m_ask_order.status >= ACKED) ? m_ask_order.price : 0; }

    bool isBidOrder(uint32_t id) { return id == m_bid_order.id; }
    bool isBidOrder(char side) { return side == 'B'? true: false; }
    bool isAskOrder(char side) { return !isBidOrder(side); }


    void sendOrder(Order &order, Action action);
    void removeQuote(Order &order);
    void updateQuotes();
    bool updateQuoteBid();
    bool updateQuoteAsk();

    bool computeQuotePrice(double price);

    using QuoteEventQueue = std::unordered_map<QuoteEvent, std::pair< std::chrono::time_point<std::chrono::system_clock> , double>>;
    std::mutex  event_mutex;
    std::condition_variable event_cv;
    QuoteEventQueue m_event_queue;

    void pushEvent(QuoteEvent e, double price=0);
    void consumeEvent();
    void removeDuplicateEvents(QuoteEventQueue &events);
    void processEvent(QuoteEventQueue &&events);


public:
  InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution);
  void OnTheoreticalPrice( double theoreticalPrice);
  void OnBestBidOffer( double bidPrice, double offerPrice);
  void OnOrderAddConfirm( uint32_t id);
  void OnOrderRemoveConfirm( uint32_t id);
  void notify(uint32_t id, uint8_t action);
  bool i_am_done = false;
};

InstrumentQuoter::InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution)
    :m_quoteoffset(quoteOffset), m_quotevolume(quoteVolume), m_tickwidth(tickWidth), m_execution(execution), m_feedcode(feedcode)
{
    std::cout<<getTime()<<"InstrumentQuoter created on symbol "<<m_feedcode<<'\n';

    m_bid_order.side = 'B';
    m_bid_order.status= DONE;

    m_ask_order.side = 'S';
    m_ask_order.status= DONE;

    consumeEvent();
}

bool InstrumentQuoter::willHitMarket(const Order &order)
{
    if(isBidOrder(order.side) && !is_zero(m_market_ask) && greater_than_equal(order.price, m_market_ask))
        return true;
    else if(isAskOrder(order.side) && !is_zero(m_market_bid) && less_than_equal(order.price, m_market_bid))
        return true;
    return false;
}

void InstrumentQuoter::sendOrder(Order &order, Action action)
{
    if( action == NEW_ORDER && order.status == NEW )
    {
        if(willHitMarket(order))
        {
            std::cout<<getTime()<<order<<" will hit the market, so not sending\n";
            order.price = 0;
            return;
        }
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
    std::cout<<getTime()<<"New Ack :"<<order<<" -> ACKED"<<'\n';;
    if(order.send_cancel_order_on_new_order_ack)
    {
       order.send_cancel_order_on_new_order_ack = false;
       order.status = ACKED;
       sendOrder(order, CANCEL_ORDER);
    }
    else
    {
       order.status = ACKED;
    }
}

void InstrumentQuoter::OnOrderRemoveConfirm( uint32_t id)
{
    Order &order = isBidOrder(id) ? m_bid_order : m_ask_order;
    std::cout<<getTime()<<"Can Ack "<<order<<" -> CANCELED "<<'\n';
    auto event = order.notify_event;
    order.reset();
    if( event != QuoteEvent::NONE_EVENT )
    {
        pushEvent(event);
    }
}
void InstrumentQuoter::OnBestBidOffer( double bidPrice, double offerPrice)
{
    m_market_bid = bidPrice;
    m_market_ask = offerPrice;
    pushEvent(QuoteEvent::DEPTH_UPDATE);
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
    }
}

bool InstrumentQuoter::updateQuoteBid()
{
    bool bid_sent=false;
    if( m_bid_order.status ==  DONE)
    {
        bid_sent = true;
        m_bid_order.status = NEW;
        m_bid_order.price = m_computed_bid;
        sendOrder(m_bid_order, NEW_ORDER);
    }
    else if ( m_bid_order.status == ACKED )
    {
        bid_sent = true;
        m_bid_order.notify_event = QuoteEvent::BID_ORDER_CANCELED;
        sendOrder(m_bid_order, CANCEL_ORDER);
    }
    else if( m_bid_order.status == PENDING_NEW )
    {
        bid_sent = true;
        m_bid_order.send_cancel_order_on_new_order_ack = true;
        m_bid_order.notify_event = QuoteEvent::BID_ORDER_CANCELED;
    }
    else if( m_bid_order.status == PENDING_CANCEL )
    {
        bid_sent = true;
        m_bid_order.notify_event = QuoteEvent::BID_ORDER_CANCELED;
    }
    return bid_sent;
}

bool InstrumentQuoter::updateQuoteAsk()
{
    bool ask_sent = false;
    if( m_ask_order.status == DONE)
    {
        ask_sent = true;
        m_ask_order.status = NEW;
        m_ask_order.price = m_computed_ask;
        sendOrder(m_ask_order, NEW_ORDER);
    }
    else if ( m_ask_order.status == ACKED )
    {
        ask_sent = true;
        m_ask_order.notify_event = QuoteEvent::ASK_ORDER_CANCELED;
        sendOrder(m_ask_order, CANCEL_ORDER);
    }
    else if(  m_ask_order.status  == PENDING_NEW )
    {
        ask_sent = true;
        m_ask_order.send_cancel_order_on_new_order_ack = true;
        m_ask_order.notify_event = QuoteEvent::ASK_ORDER_CANCELED;
    }
    else if( m_ask_order.status == PENDING_CANCEL )
    {
        ask_sent = true;
        m_ask_order.notify_event = QuoteEvent::ASK_ORDER_CANCELED;
    }
    return ask_sent;
}

void InstrumentQuoter::updateQuotes()
{
    bool bid_sent=false, ask_sent = false;
    double prev_ask = m_ask_order.price;
    if( is_zero(prev_ask) || less_than(m_computed_bid, prev_ask)) // bid is not hitting previous_ask
    {
        bid_sent = updateQuoteBid();
        ask_sent = updateQuoteAsk();
    }
    else 
    {
        ask_sent = updateQuoteAsk();
        bid_sent = updateQuoteBid();
    }
}

void InstrumentQuoter::OnTheoreticalPrice(double theoreticalPrice)
{
    m_theo = theoreticalPrice;
    if( is_zero(theoreticalPrice) )
        pushEvent(QuoteEvent::THEO_ERROR, theoreticalPrice);
    else
        pushEvent(QuoteEvent::THEO_UPDATE, theoreticalPrice);
}

Execution::Execution()
{
    std::thread([&,this]()
    {
        std::cout<<getTime()<<"OrderEvent exchange is running\n";
        decltype(m_events) tmp;
        while( running )
        {
            tmp.clear();
            {
                std::unique_lock lock(cv_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1000), [this]{return !m_events.empty();});
                tmp.swap(m_events);
            }
            for(auto event: tmp)
                if( m_observer)
                    m_observer->notify(event.first, event.second);
        }
        for(auto event: m_events)
            if( m_observer)
                m_observer->notify(event.first, event.second);

        std::cout<<getTime()<<"OrderEvent exchange is stopped\n";
        i_am_done = true;
    }).detach();
}

void InstrumentQuoter::notify(uint32_t id, uint8_t action)
{ 
    if( action == 1) 
        OnOrderAddConfirm(id); 
    else 
        OnOrderRemoveConfirm(id); 
}

void InstrumentQuoter::pushEvent(QuoteEvent e, double price)
{
    {
        std::lock_guard lock(event_mutex);
        std::chrono::time_point<std::chrono::system_clock> cur = std::chrono::system_clock::now();
        m_event_queue[e] = std::make_pair(cur, price);
    }
    event_cv.notify_one();
}

void InstrumentQuoter::consumeEvent()
{
    std::thread([&,this]
    {
        std::cout<<getTime()<<"OrderEvent consumer is running\n";
        QuoteEventQueue tmp_events;
        while(running)
        {
            tmp_events.clear();
            {
                std::unique_lock lock(event_mutex);
                event_cv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return !m_event_queue.empty();});
                tmp_events.swap(m_event_queue);
            }
            processEvent(std::move(tmp_events));
        }
        std::cout<<getTime()<<"OrderEvent consumer is stopped\n";
        i_am_done = true;
    }).detach();
}

void InstrumentQuoter::removeDuplicateEvents(QuoteEventQueue &events)
{
    // both THEO_ERR and THE_UPDATE can't exis the queue same time
    auto ite_theo_err = events.find(QuoteEvent::THEO_ERROR);
    if( ite_theo_err == events.end() )
        return;
    auto ite_theo_upd = events.find(QuoteEvent::THEO_UPDATE);
    if ( ite_theo_upd == events.end() )
        return;
    if ( ite_theo_err->second.second > ite_theo_upd->second.second) // latest upate the theo_error
        events.erase(ite_theo_upd);
    else
        events.erase(ite_theo_err); // theo_update is the lastest update
}

void InstrumentQuoter::processEvent(QuoteEventQueue &&events)
{
    bool skip_theo_error = false;
    removeDuplicateEvents(events);
    for(auto event: events)
    {
        if( event.first == QuoteEvent::THEO_UPDATE) //new theo price update
        {
            std::cout<<getTime()<<"THEO_UPDATE "<<event.second.second<<'\n';
            if ( computeQuotePrice(event.second.second))
            {
                updateQuotes();
            }
        }
        else if ( event.first == QuoteEvent::THEO_ERROR)
        {
            removeQuote(m_bid_order);
            removeQuote(m_ask_order);
            // no further event processing needed
            break;
        }
        else if ( event.first == QuoteEvent::BID_ORDER_CANCELED)
        {
            updateQuoteBid(); 
        }
        else if ( event.first == QuoteEvent::ASK_ORDER_CANCELED)
        {
            updateQuoteAsk();
        }
    }
}

bool InstrumentQuoter::computeQuotePrice(double price)
{
    double bid = getDownTickPrice(price - m_quoteoffset, m_tickwidth);
    double ask = getUpTickPrice  (price + m_quoteoffset, m_tickwidth);
    if( greater_than_equal(bid, ask) || is_zero(bid) || is_zero(ask) )
    {
        m_computed_bid = 0; 
        m_computed_ask = 0;
        std::cerr<<"Error in the theo price calcuation "<< bid<< ">=" << ask <<" on theo "<<price<<'\n';
        return false;
    }

    //Assumed both bid and ask price changes on theo update
    double quoted_bid = getOwnBidQuotedPrice();
    double quoted_ask = getOwnAskQuotedPrice();
    if( are_equal(bid, quoted_bid) || are_equal(ask, quoted_ask)) // new price is/are same as prev price
        return false;

    m_computed_bid = bid; 
    m_computed_ask = ask;

    return true;
}

void test1()
{
    Execution handle;
    InstrumentQuoter iq("0001.HK", 0.20, 100, 0.05, handle);
    handle.setObserver(&iq);
    iq.OnBestBidOffer(9.80,10.20);
    iq.OnTheoreticalPrice(10);
    iq.OnTheoreticalPrice(9.950);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    iq.OnTheoreticalPrice(10.1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    iq.OnTheoreticalPrice(9.950);
    while ( !handle.i_am_done || !iq.i_am_done )
        std::this_thread::yield();
}

int main()
{
    test1();
    return 0;
}
