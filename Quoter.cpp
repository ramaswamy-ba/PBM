#include <iostream>
#include <cstdint>
#include <cmath>

/* Assumption:
  * Computed TheoriticalPrice comes after the bid and offer price received
  * On every TheoriticalPrice update quoted_bid and quoted_ask price should be computed and order sent to market, based on the condition
  * quoted_bid < Theoriticalprice < quoted_ask
  * ticksize is same across whole price range
  * Here is single instrument, so classes are not decomposed as part of the class hierarchy
  * we don't send multiple orders at the same side, at a time there is only one  buy and sell order at each side
  * sequence of order ack is guaranteed as per the sequence of order sent
  *There is no market bid volume and ask volume info,  so while cancelling and inserting the order, we don't know are the only one in the market or many on the same price
*/

namespace tick_util
{
    constexpr double epsilon_value = 1e-6;
    bool is_zero(double a )                     { return std::fabs(a) < epsilon_value; }
    bool are_equal(double a, double b)          { return std::fabs(a - b) < epsilon_value; }
    bool less_than(double a, double b)          { return (b - a) > epsilon_value; }
    bool greater_than(double a, double b)       { return (a - b) > epsilon_value; }
    bool less_than_equal(double a, double b)    { return a <= (b + epsilon_value); }
    bool greater_than_equal(double a, double b) { return (a + epsilon_value) <= b; }

    double getDownTickPrice(double price, double tick) { return std::floor( price / tick + epsilon_value) * tick; }
    double getUpTickPrice  (double price, double tick) { return std::ceil(price / tick - epsilon_value ) * tick ; }
    double getNearestToTick(double price, double tick) { return std::floor(price / tick + 0.5) * tick; }
    bool   willHitPrice(double p1, double p2)          { return greater_than_equal(p1, p2); }
}
using namespace tick_util;
class Execution
{
public:
  void requestOrderAdd( uint32_t id, std::string const &feedcode, char orderSide, double orderPrice, uint32_t orderVolume) {
    std::cout<<"New Order "<<id<<' '<<orderSide<<' '<<orderPrice<<' '<<orderVolume<<'\n';
  }
  void requestOrderRemove( uint32_t id){ std::cout<<"Can Order "<<id<<'\n'; }
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

    enum Status
    {
        NEW=1,
        ACKED,
        PENDING_NEW,
        PENDING_CANCEL,
        DONE,
        NONE
    };
    enum Action { NEW_ORDER, CANCEL_ORDER};
    struct Order
    {
        uint32_t id;
        char side;
        double price=0;
        Status status;
        bool send_new_order_on_cancel_ack=false;
        bool send_cancel_order_on_new_order_ack = false;
    };

    Order m_bid_order, m_ask_order;
    void sendOrder(Order &order, Action action);
public:
  InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution);
  void OnTheoreticalPrice( double theoreticalPrice);
  void OnBestBidOffer( double bidPrice, double offerPrice);
  void OnOrderAddConfirm( uint32_t id);
  void OnOrderRemoveConfirm( uint32_t id);

};

InstrumentQuoter::InstrumentQuoter( std::string const &feedcode, double quoteOffset, uint32_t quoteVolume, double tickWidth, Execution &execution)
    :m_quoteoffset(quoteOffset), m_quotevolume(quoteVolume), m_tickwidth(tickWidth), m_execution(execution), m_feedcode(feedcode)
{
    std::cout<<"InstrumentQuoter created on symbol "<<m_feedcode<<'\n';

    m_bid_order.side = 'B';
    m_bid_order.status= NONE;

    m_ask_order.side = 'S';
    m_ask_order.status= NONE;
}
void InstrumentQuoter::sendOrder(Order &order, Action action)
{
    if( action == NEW_ORDER)// && order.status == NEW )
    {
        order.id=getOrderId();
        m_execution.requestOrderAdd(order.id, m_feedcode, order.side, order.price, m_quotevolume);
        order.status = PENDING_NEW;
    }
    else if ( action == CANCEL_ORDER)// && order.status == ACKED )
    {
        m_execution.requestOrderRemove(order.id);
        order.status = PENDING_CANCEL;
    }
}

void InstrumentQuoter::OnOrderAddConfirm(uint32_t id)
{
    Order *o = id == m_bid_order.id ? &m_bid_order : &m_ask_order;
    std::cout<<"New Ack "<<o->id<<' '<<o->price<<' '<<o->side<<'\n';
    if ( id == m_bid_order.id)
    {
        m_quote_bid = m_bid_order.price;
        m_bid_order.status = ACKED;
        if(m_bid_order.send_cancel_order_on_new_order_ack )
        {
           m_bid_order.send_cancel_order_on_new_order_ack = false;
           sendOrder(m_bid_order, CANCEL_ORDER);
        }
    }
    else
    {
        m_quote_ask = m_ask_order.price;
        m_ask_order.status = ACKED;
        if( m_ask_order.send_cancel_order_on_new_order_ack)
        {
            m_ask_order.send_cancel_order_on_new_order_ack = false;
            sendOrder(m_ask_order, CANCEL_ORDER);
        }
    }
}

void InstrumentQuoter::OnOrderRemoveConfirm( uint32_t id)
{
    Order *o = id == m_bid_order.id ? &m_bid_order : &m_ask_order;
    std::cout<<"Can Ack "<<o->id<<' '<<o->price<<' '<<o->side<<'\n';
    if ( id == m_bid_order.id)
    {
        m_bid_order.status = DONE;
        m_quote_bid = 0;

        if( m_bid_order.send_new_order_on_cancel_ack )
        {
            m_bid_order.send_new_order_on_cancel_ack = false;
            if( less_than(m_bid_order.price, m_market_ask))
                sendOrder(m_bid_order, NEW_ORDER);
        }
    }
    else
    {
        m_ask_order.status = DONE;
        m_quote_ask = 0;

        if( m_ask_order.send_new_order_on_cancel_ack)
        {
            m_bid_order.status = NEW;
            m_ask_order.send_new_order_on_cancel_ack = false;
            if(greater_than(m_ask_order.price, m_market_bid))
                sendOrder(m_ask_order, NEW_ORDER);
        }
    }
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
    if( !is_zero(m_quote_bid) && are_equal(price,  m_market_bid)) // new price is same as prev price
        return false;
    if( !is_zero(m_market_ask) && greater_than(price,  m_market_ask))
        return false;
    //if( !is_zero(m_quote_ask) && greater_than(price, m_quote_ask))
    //    return false;
    return true;
}

bool InstrumentQuoter::isValidAskPrice( double price)
{
    if( is_zero(price))
        return false;
    if( !is_zero(m_quote_ask) && are_equal(price,  m_market_ask))
        return false;
    if( !is_zero(m_quote_bid) && less_than(price,  m_market_bid))
        return false;
    //if( !is_zero(m_quote_bid) && less_than(price, m_quote_bid))
    //    return false;
    return true;
}

void InstrumentQuoter::OnTheoreticalPrice(double theoreticalPrice)
{
    m_theo = theoreticalPrice;
    if( is_zero(m_theo) )
    {   std::cout<<"The reset cancelling bid/ask orders\n";
        //cancel orders and return;
        if ( isBidQuoted() )
            sendOrder(m_bid_order, CANCEL_ORDER);
        else if ( m_bid_order.status  == PENDING_NEW )
            m_bid_order.send_cancel_order_on_new_order_ack = true;

        if ( isAskQuoted() )
            sendOrder(m_ask_order, CANCEL_ORDER);
        else if ( m_ask_order.status == PENDING_NEW )
            m_ask_order.send_cancel_order_on_new_order_ack = true;
            
        return;
    }

    double bid = getDownTickPrice(m_theo - m_quoteoffset, m_tickwidth);
    double ask = getUpTickPrice  (m_theo + m_quoteoffset, m_tickwidth);

    bool bid_not_sent=true, ask_not_sent=true;
    if( isValidAskPrice(ask) )
    {
        if( isAskQuoted() ) //quote exist in the market
        {
            ask_not_sent = false;
            sendOrder(m_ask_order, CANCEL_ORDER); // remove the ask order
        }

        if ( isBidQuoted() && less_than_equal(ask, m_quote_bid) ) //new quote ask price is crossing with our own bid in the market
        {
            bid_not_sent = false;
            sendOrder(m_bid_order, CANCEL_ORDER);
        }

        m_ask_order.price = ask;
        if( ask_not_sent )
        {
            if( is_zero(m_market_bid) || greater_than(ask, m_market_bid)  )  // no own ask in the market and new ask price not hitting the market
            {
                m_ask_order.status = NEW;
                ask_not_sent = false;
                sendOrder(m_ask_order, NEW_ORDER);
            }
        }
        else// if( m_ask_order.status == PENDING_NEW )
        {
            m_ask_order.send_new_order_on_cancel_ack = true;
        }
    }
    
    if( isValidBidPrice(bid) )
    {
        if( isBidQuoted() ) // quote exit in the market
        {
            sendOrder(m_bid_order, CANCEL_ORDER);
            bid_not_sent = false;
        }
        if ( ask_not_sent && isAskQuoted() && greater_than_equal(bid, m_quote_ask) )
            sendOrder(m_ask_order, CANCEL_ORDER); // remove the ask order

        m_bid_order.price = bid;
        if( bid_not_sent )
        {
            if( is_zero(m_market_ask) || less_than(bid, m_market_ask) ) // no own bid in the market and new bid price not hitting the market
            {
                m_bid_order.status = NEW;
                bid_not_sent = false;
                sendOrder(m_bid_order, NEW_ORDER);
            }
        }
        else
        {
            m_bid_order.send_new_order_on_cancel_ack = true;
        }
    }
}

int main()
{
    Execution handle;
    InstrumentQuoter iq("0001.HK", 0.20, 100, 0.05, handle);
    iq.OnBestBidOffer(9.80,10.20);
    iq.OnTheoreticalPrice(10);
    iq.OnOrderAddConfirm(1); iq.OnOrderAddConfirm(2);
    iq.OnTheoreticalPrice(9.950);
    iq.OnOrderRemoveConfirm(1); iq.OnOrderRemoveConfirm(2);
    iq.OnOrderAddConfirm(3); iq.OnOrderAddConfirm(4);
    return 0;
}
